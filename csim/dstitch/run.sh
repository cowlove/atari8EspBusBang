#!/bin/bash 
usage() {
	cat <<'EOF'
Usage: ./run.sh [OPTIONS] [SMB_URL]

Build and run Atari800 and the FUSE mount as child processes. The main shell
waits for either child to exit; Ctrl-C or `-t` stops both processes started here.

Options:
  -h, --headless     Run Atari800 under `xvfb-run -a`
  -t <sec>           Stop the run after the given number of seconds
  --sdfs-sectors N   Set SDFS image geometry (64..65535 sectors; default 65535)
  --sdfs-write-poc   Enable experimental SDFS create/append write-through
  --help             Show this help and exit

SMB_URL defaults to smb://localhost/foo. Atari800 output is written to
atari800.log (overwritten on each run).
EOF
}

headless=0
timeout_seconds=""
sdfs_sectors=""
sdfs_write_poc=0
SMB_URL="${SMB_URL:=file://./hostdir/}"
#-smb://localhost/foo}
url_set=0
parse_options=1

while [ "$#" -gt 0 ]; do
	if [ "$parse_options" -eq 0 ]; then
		if [ "$url_set" -eq 1 ]; then
			echo "Only one SMB_URL may be supplied." >&2
			usage >&2
			exit 2
		fi
		SMB_URL=$1
		url_set=1
		shift
		continue
	fi

	case "$1" in
		--help)
			usage
			exit 0
			;;
		--sdfs-sectors)
			if [ "$#" -lt 2 ]; then
				echo "--sdfs-sectors requires a sector count." >&2
				usage >&2
				exit 2
			fi
			sdfs_sectors=$2
			shift
			;;
		-t|--timeout)
			if [ "$#" -lt 2 ] || [[ ! "$2" =~ ^0*[1-9][0-9]*$ ]]; then
				echo "$1 requires a positive integer number of seconds." >&2
				usage >&2
				exit 2
			fi
			timeout_seconds=$2
			shift
			;;
		--sdfs-sectors=*)
			sdfs_sectors=${1#*=}
			;;
		--sdfs-write-poc)
			sdfs_write_poc=1
			;;
		-h|--headless)
			headless=1
			;;
		--)
			parse_options=0
			;;
		*)
			if [ "$parse_options" -eq 1 ] && [[ "$1" == -* ]]; then
				echo "Unknown option: $1" >&2
				usage >&2
				exit 2
			fi
			if [ "$url_set" -eq 1 ]; then
				echo "Only one SMB_URL may be supplied." >&2
				usage >&2
				exit 2
			fi
			SMB_URL=$1
			url_set=1
			;;
	esac
	shift
done

if [ "$headless" -eq 1 ] && ! command -v xvfb-run >/dev/null 2>&1; then
	echo "--headless requires xvfb-run (install the xvfb package)." >&2
	exit 1
fi

: "${CDEFS:=-DLOG=printf -DSECTOR_SIZE=256}"
export CDEFS
rm -f ~/tmp/hostdir/*
rm -f ./hostdir/*
fusermount -u ./mnt
make clean
make || exit

rm -f ./fuse_d1.atr 
atr ./fuse_d1.atr mkfs dos2.0d
atr ./fuse_d1.atr put -l autoexec.bat 
cat /dev/random | od | head -201 > foo
cat /dev/random | od | head -301 > bar
echo -n 1 > onebyte
atr ./fuse_d1.atr put -l foo 
atr ./fuse_d1.atr put -l bar 
atr ./fuse_d1.atr put -l onebyte
cat foo onebyte foo | tr "\n" "\233" > FOO.expected
mount_sector_args=()
if [ -n "$sdfs_sectors" ]; then
	mount_sector_args=(--sdfs-sectors "$sdfs_sectors")
fi
sdfs_write_args=(--sdfs-trace-overlay)
if [ "$sdfs_write_poc" -eq 1 ]; then
	sdfs_write_args=(--sdfs-write-poc)
fi

atari_pid=""
mount_pid=""
timer_pid=""
cleanup_children() {
	for pid in "$atari_pid" "$mount_pid" "$timer_pid"; do
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
			kill "$pid" 2>/dev/null || true
		fi
	done
	if [ -n "$atari_pid" ]; then wait "$atari_pid" 2>/dev/null || true; fi
	if [ -n "$mount_pid" ]; then wait "$mount_pid" 2>/dev/null || true; fi
	if [ -n "$timer_pid" ]; then wait "$timer_pid" 2>/dev/null || true; fi
	atari_pid=""
	mount_pid=""
	timer_pid=""
}
trap cleanup_children EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./libsmb2/build/lib ./dstitch_mount --sdfs \
	"${sdfs_write_args[@]}" "${mount_sector_args[@]}" "$SMB_URL" -f ./mnt &
mount_pid=$!
sleep 1
if ! kill -0 "$mount_pid" 2>/dev/null; then
	wait "$mount_pid"
	mount_status=$?
	cat atari800.log
	exit "$mount_status"
fi
(
	sleep 1
	if [ "$headless" -eq 1 ]; then
		exec env ATARI800_HEADLESS=1 xvfb-run -a ./atari800.sh
	else
		exec ./atari800.sh
	fi
) > ./atari800.log 2>&1 &
atari_pid=$!
if [ -n "$timeout_seconds" ]; then
	sleep "$timeout_seconds" &
	timer_pid=$!
	wait -n "$atari_pid" "$mount_pid" "$timer_pid"
	if ! kill -0 "$timer_pid" 2>/dev/null; then
		echo "run.sh: timed exit after $timeout_seconds seconds" >&2
		run_status=124
	else
		run_status=$?
	fi
else
	wait -n "$atari_pid" "$mount_pid"
	run_status=$?
fi
cat atari800.log
if cmp FOO.expected ./hostdir/FOO; then
	echo "RESULT: FOO comparison success"
else
	echo "RESULT: FOO comparison FAILED"
	if [ "$run_status" -eq 0 ]; then run_status=1; fi
fi
exit "$run_status"
