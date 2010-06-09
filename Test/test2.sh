#!/bin/bash
X264_MIN_HEART_RATE=5
X264_MAX_HEART_RATE=10
THREAD_COUNT=1
while getopts 'm:M:t:a:f:p:' OPT; do
	case $OPT in
		m)
			X264_MIN_HEART_RATE=$OPTARG;;
		M)
			X264_MAX_HEART_RATE=$OPTARG;;
		t)
			THREAD_COUNT=$OPTARG;;
		a)
			AFFINITY=$OPTARG;;
		f)
			FREQ=$OPTARG;;
		p)
			PROG=$OPTARG;;
	esac
	echo $OPT $OPTARG
done
export X264_MIN_HEART_RATE
export X264_MAX_HEART_RATE

case $PROG in
	fs|frequencyscaler)
		PROGRAM=../bin/frequencyscaler1
		PROGSYM=fs;;
	ca|coreallocator)
		PROGRAM=../bin/core-allocator
		PROGSYM=ca;;
	co|combo|combined)
		PROGRAM=../bin/combined
		PROGSYM=co;;
esac

if [ -z $PROGRAM ]; then
	echo a service must be specified
	exit 1
fi

if [ ! -z $FREQ ]; then
	for i in 0 1 2 3; do cpufreq-set -c $i -f ${FREQ}GHz; done
fi

../x264-heartbeat-shared/x264 -B 400 --threads $THREAD_COUNT -o out.264 pipe.y4m >/dev/null & mplayer -nolirc ../../video-x264/tractor.mkv -vo yuv4mpeg:file=pipe.y4m -nosound > /dev/null &

if [ ! -z $AFFINITY ]; then
	taskset -pc $AFFINITY $(ls ../HB)
fi

LOGNAME=${PROGSYM}_hr${X264_MIN_HEART_RATE}-${X264_MAX_HEART_RATE}_t${THREAD_COUNT}_a${AFFINITY}_f${FREQ}.txt
echo Sending output to $LOGNAME
$PROGRAM 240 $AFFINITY| tee $LOGNAME
