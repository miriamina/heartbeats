/*
 *  combined.c
 *  
 *
 *  Created by Camillo Lugaresi on 04/06/10.
 *
 */

#include <sys/errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <cpufreq.h>
#include "heart_rate_monitor.h"

/*
 The best part of C is macros. The second best part of C is goto.
 */
#define fail_if(exp, msg) do { if ((exp)) { fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, (msg), strerror(errno)); goto fail; } } while (0)

#define ACTUATOR_CORE_COUNT 1
#define ACTUATOR_GLOBAL_FREQ 2
#define ACTUATOR_SINGLE_FREQ 3

typedef struct actuator actuator_t;
struct actuator {
	int id;
	pid_t pid;
	int core;
	int (*init_f) (actuator_t *act);
	int (*action_f) (actuator_t *act);
	uint64_t value;
	uint64_t set_value;
	uint64_t min;
	uint64_t max;
	void *data;
};

typedef void (*decision_function_t) (heartbeat_record_t *hb, int act_count, actuator_t *acts);

typedef struct freq_scaler_data {
	unsigned long *freq_array;
	int freq_count;
	int cur_index;
} freq_scaler_data_t;

heart_rate_monitor_t hrm;
char *heartbeat_dir;

/* this is way simpler than spawning a process! */
int get_heartbeat_apps(int *pids, int maxcount)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;
	int pid;
	char *end;
	
	dir = opendir(heartbeat_dir);
	fail_if(dir == NULL, "cannot open heartbeat dir");
	while (((entry = readdir(dir)) != NULL) && count < maxcount) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		pid = strtol(entry->d_name, &end, 10);
		if (*end == 0) pids[count++] = pid;
		else fprintf(stderr, "file name is not a pid: %s\n", entry->d_name);
	}
	(void)closedir(dir);
	return count;
fail:
	return -1;
}

/* core allocator stuff */

int get_core_count ()
{
	static int count = 0;
	FILE *fp = NULL;

	if (!count) {
		char buf[256];

		fp = fopen("/proc/cpuinfo", "r");
		fail_if(!fp, "cannot open /proc/cpuinfo");
		while (fgets(buf, sizeof(buf), fp))
			if (strstr(buf, "processor"))
				count++;
		fclose(fp);
fail:
		if (count < 1) count = 1;
	}
	return count;
}

int core_init (actuator_t *act)
{
	char buf[256];
	FILE *proc;
	unsigned int affinity;
	
	snprintf(buf, sizeof(buf), "taskset -p %d | sed 's/.* //'", (int)act->pid);
	proc = popen(buf, "r");
	fail_if(!proc, "cannot read initial processor affinity");
	fail_if(fscanf(proc, "%x", &affinity) < 1, "cannot parse initial processor affinity");
	pclose(proc);
	
	act->value = 0;
	while (affinity) {
		act->value += affinity & 0x01;
		affinity /= 2;
	}
	act->set_value = act->value;
	act->min = 1;
	act->max = get_core_count();

	return 0;
fail:
	return -1;
}

int core_act (actuator_t *act)
{
	char command[256];
	int err;
	
	snprintf(command, sizeof(command), "taskset -pc 0-%d %d > /dev/null", (int)(act->set_value - 1), (int)act->pid);
	err = system(command);
	if (!err)
		act->value = act->set_value;
	return err;
}

/* frequency scaler stuff */

int create_freq_array(struct cpufreq_available_frequencies *freq_list, unsigned long **freq_array_p)
{
	struct cpufreq_available_frequencies *freq = freq_list;
	int n = 0;
	unsigned long *f;
	
	while (freq) {
		n++;
		freq = freq->next;
	}
	f = *freq_array_p = (unsigned long *) malloc(n * sizeof(unsigned long));
	fail_if(!f, "cannot allocate freq array");
	freq = freq_list;
	while (freq) {
		*f++ = freq->frequency;
		freq = freq->next;
	}
	return n;
fail:
	return -1;
}

int get_freq_index(freq_scaler_data_t *data, unsigned long freq)
{
	int i;
	
	for (i = 0; i < data->freq_count; i++)
		if (data->freq_array[i] == freq)
			return i;
	return -1;
}

int single_freq_init (actuator_t *act)
{
	int err;
	struct cpufreq_policy *policy;
	struct cpufreq_available_frequencies *freq_list;
	freq_scaler_data_t *data;
	unsigned long freq_min, freq_max;
	
	act->data = data = malloc(sizeof(freq_scaler_data_t));
	fail_if(!data, "cannot allocate freq data block");
	
	err = cpufreq_get_hardware_limits(act->core, &freq_min, &freq_max);
	fail_if(err, "cannot get cpufreq hardware limits");
	act->min = freq_min;
	act->max = freq_max;
	
	policy = cpufreq_get_policy(act->core);
	fail_if(!policy, "cannot get cpufreq policy");
	if (strcmp(policy->governor, "userspace") != 0) {
		err = cpufreq_modify_policy_governor(act->core, "userspace");
		policy = cpufreq_get_policy(act->core);
		fail_if (strcmp(policy->governor, "userspace") != 0, "cannot set cpufreq policy to userspace");
	}
	
	freq_list = cpufreq_get_available_frequencies(act->core);
	data->freq_count = create_freq_array(freq_list, &data->freq_array);
	fail_if(data->freq_count < 1, "cannot get frequency list");
	
	act->value = act->set_value = cpufreq_get_freq_kernel(act->core);
	data->cur_index = get_freq_index(data, act->value);
	
	return 0;
fail:
	return -1;
}

int global_freq_init (actuator_t *act)
{
	int err;
	
	act->core = 0;	/* just get all data from the first cpu and assume they're all the same */
	err = single_freq_init(act);
	act->core = -1;
	return err;
}

int single_freq_act (actuator_t *act)
{
	int err = 0;
	
	err = cpufreq_set_frequency(act->core, act->set_value);
	/* warning: cpufreq_set_frequency tries sysfs first, then proc; this means that if
	 sysfs fails with EACCESS, the errno is then masked by the ENOENT from proc! */
	act->value = cpufreq_get_freq_kernel(act->core);
	return err;
}

int global_freq_act (actuator_t *act)
{
	int err = 0;
	int cpu;
	
	for (cpu = 0; cpu < get_core_count(); cpu++)
		err = err || cpufreq_set_frequency(cpu, act->set_value);
	/* warning: cpufreq_set_frequency tries sysfs first, then proc; this means that if
	sysfs fails with EACCESS, the errno is then masked by the ENOENT from proc! */
	act->value = cpufreq_get_freq_kernel(0);
	return err;
}

/* decision functions */

void dummy_control (heartbeat_record_t *hb, int act_count, actuator_t *acts)
{
	/* do nothing, lol */
}

void core_heuristics (heartbeat_record_t *current, int act_count, actuator_t *acts)
{
	static actuator_t *core_act = NULL;
	int i;
	for (i = 0; i < act_count && core_act == NULL; i++)
		if (acts[i].id == ACTUATOR_CORE_COUNT)
			core_act = &acts[i];
	
	if (current->window_rate < hrm_get_min_rate(&hrm)) {
		if (core_act->value < core_act->max) core_act->set_value++;
	}
	else if(current->window_rate > hrm_get_max_rate(&hrm)) {
		if (core_act->value > core_act->min) core_act->set_value--;
	}
}

void freq_heuristics (heartbeat_record_t *current, int act_count, actuator_t *acts)
{
	static actuator_t *freq_act = NULL;
	freq_scaler_data_t *freq_data;
	int i;
	for (i = 0; i < act_count && freq_act == NULL; i++)
		if (acts[i].id == ACTUATOR_GLOBAL_FREQ)
			freq_act = &acts[i];
	freq_data = freq_act->data;
	
	if (current->window_rate < hrm_get_min_rate(&hrm)) {
		if (freq_data->cur_index > 0) {
			freq_data->cur_index--;
			freq_act->set_value = freq_data->freq_array[freq_data->cur_index];
		}
	}
	else if(current->window_rate > hrm_get_max_rate(&hrm)) {
		if (freq_data->cur_index < freq_data->freq_count-1) {
			freq_data->cur_index++;
			freq_act->set_value = freq_data->freq_array[freq_data->cur_index];
		}
	}
}

void uncoordinated_heuristics (heartbeat_record_t *current, int act_count, actuator_t *acts)
{
	core_heuristics(current, act_count, acts);
	freq_heuristics(current, act_count, acts);
}

void step_heuristics (heartbeat_record_t *current, int act_count, actuator_t *acts)
{
	static actuator_t *core_act = NULL;
	static actuator_t *freq_acts[16];
	int core_count, last_core;
	freq_scaler_data_t *freq_data;	

	if (!core_act) {
		for (i = 0; i < act_count; i++) {
			if (acts[i].id == ACTUATOR_CORE_COUNT)
				core_act = &acts[i];
			else if (acts[i].id == ACTUATOR_SINGLE_FREQ && acts[i].core <= 16) {
				freq_acts[acts[i].core] = &acts[i];
			}
		}
		core_count = core_act->max;
		if (core_count > 16) exit(2);
	}
	
	last_core = core_act->value - 1;
	freq_data = freq_acts[last_core]->data;
	
	if (current->window_rate < hrm_get_min_rate(&hrm)) {
		if (freq_data->cur_index > 0) {
			/* increase last core's frequency if possible */
			freq_data->cur_index--;
			freq_acts[last_core]->set_value = freq_data->freq_array[freq_data->cur_index];
		} else if (last_core < core_act->max - 1) {
			/* else, add another core... */
			core_act->set_value = core_act->value + 1;
			last_core++;
			/* ...at the lowest initial frequency */
			freq_data = freq_acts[last_core]->data;
			freq_data->cur_index = freq_data->freq_count-1;
			freq_acts[last_core]->set_value = freq_data->freq_array[freq_data->cur_index];
		}
	}
	else if(current->window_rate > hrm_get_max_rate(&hrm)) {
		if (freq_data->cur_index < freq_data->freq_count-1) {
			/* decrease last core's frequency if possible */
			freq_data->cur_index++;
			freq_acts[last_core]->set_value = freq_data->freq_array[freq_data->cur_index];
		} else if (last_core > core_act->min - 1) {
			/* else, reduce core count */
			core_act->set_value = core_act->value - 1;
			last_core--;
			/* the core that is now last should already be at max frequency */
		}
	}
	
}

/* BACK TO ZA CHOPPA */

void print_status(heartbeat_record_t *current, int64_t skip_until_beat, char action, int act_count, actuator_t *controls)
{
	int i;

	printf("%lld\t%.3f\t%lld\t%c", (long long int)current->beat, current->window_rate, (long long int)skip_until_beat, action);
	for (i = 0; i < act_count; i++)
		printf("\t%lld", controls[i].value);
	printf("\n");
}

int main(int argc, char **argv)
{
	int n_apps = 0;
	int apps[16];
	int err;
	int i;
	int64_t window_size;
	int64_t skip_until_beat = 0;
	int64_t last_beat = 0;
	heartbeat_record_t current;
	int core_count, actuator_count;
	actuator_t *controls;
	decision_function_t decision_f;
	int acted;

	/* we want to see this in realtime even when it's piped through tee */
	setlinebuf(stdout);
	
	/* setupping arbit */
	heartbeat_dir = getenv("HEARTBEAT_ENABLED_DIR");
	fail_if(heartbeat_dir == NULL, "environment variable HEARTBEAT_ENABLED_DIR undefined");
	
	while (n_apps == 0)
		n_apps = get_heartbeat_apps(apps, sizeof(apps)/sizeof(apps[0]));
	fail_if(n_apps != 1, "this service only supports a single app. please delete c:\\system32");
	printf("monitoring process %d\n", apps[0]);
	
	/* initrogenizing old river control structure */
	core_count = get_core_count();
	actuator_count = core_count + 2;
	
	controls = malloc(sizeof(actuator_t) * actuator_count);
	fail_if(!controls, "could not allocate actuators");
	for (i = 0; i < core_count; i++)
		controls[i] = (actuator_t) { .id = ACTUATOR_SINGLE_FREQ, .core = i,  .pid = -1,      .init_f = single_freq_init, .action_f = single_freq_act };
	controls[i++] =   (actuator_t) { .id = ACTUATOR_GLOBAL_FREQ, .core = -1, .pid = -1,      .init_f = global_freq_init, .action_f = global_freq_act };
	controls[i++] =   (actuator_t) { .id = ACTUATOR_CORE_COUNT,  .core = -1, .pid = apps[0], .init_f = core_init,        .action_f = core_act };
	
	for (i = 0; i < actuator_count; i++) {
		err = controls[i].init_f(&controls[i]);
		fail_if(err, "cannot initialize actuator");
	}
	decision_f = uncoordinated_heuristics;
	
	/* begin monitoration of lone protoss */
	err = heart_rate_monitor_init(&hrm, apps[0]);
	fail_if(err, "cannot start heart rate monitor");
	
	window_size = hrm_get_window_size(&hrm);
	current.beat = -1;
	
	while (1) {	/* what, me worry? */
		do {
			err = hrm_get_current(&hrm, &current);
		} while (err || current.beat <= last_beat || current.window_rate == 0.0);

		last_beat = current.beat;
		if (current.beat < skip_until_beat) {
			print_status(&current, skip_until_beat, '.', actuator_count, controls);
			continue;
		}
		
		/*printf("Current beat: %lld, tag: %d, window: %lld, window_rate: %f\n",
			   current.beat, current.tag, window_size, current.window_rate);*/
		
		decision_f(&current, actuator_count, controls);
		
		acted = 0;
		for (i = 0; i < actuator_count; i++) {
			actuator_t *act = &controls[i];
			if (act->set_value != act->value) {
				err = act->action_f(act);	/* TODO: handle error */
				if (err) fprintf(stderr, "action %d failed: %s\n", act->id, strerror(errno));
				acted = 1;
			}
		}
		skip_until_beat = current.beat + (acted ? window_size : 1);
		
		print_status(&current, skip_until_beat, acted ? '*' : '=', actuator_count, controls);
	}
	
	heart_rate_monitor_finish(&hrm);
	
	return 0;
fail:
	return 1;
}
