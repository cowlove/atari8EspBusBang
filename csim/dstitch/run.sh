#!/bin/bash 
usage() {
	cat <<'EOF'
Usage: ./run.sh [OPTIONS] [SMB_URL]

Build and run the FUSE-backed Atari800 simulator. By default, the harness
stops Atari800 and FUSE when the ATR contains a `done` file.

Options:
  -e                 Enable exit-on-done behavior (the default)
  -n, --noexit       Keep running after the `done` file appears
  -h, --headless     Run Atari800 under `xvfb-run -a`
  --sdfs-sectors N   Set SDFS image geometry (64..65535 sectors; default 65535)
  --help             Show this help and exit

SMB_URL defaults to smb://localhost/foo. Atari800 output is written to
atari800.log (overwritten on each run).
EOF
}

exit_on_done=1
headless=0
sdfs_sectors=""
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
		-e)
			exit_on_done=1
			;;
		-n|--noexit)
			exit_on_done=0
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
		--sdfs-sectors=*)
			sdfs_sectors=${1#*=}
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
fusermount -u ./mnt
make clean
make || exit

rm -f ./fuse_d1.atr 
atr ./fuse_d1.atr mkfs dos2.0d
atr ./fuse_d1.atr put -l autoexec.bat 
cat /dev/random | od | head -200 > foo
cat /dev/random | od | head -300 > bar 
atr ./fuse_d1.atr put -l foo 
atr ./fuse_d1.atr put -l bar 

# By default, poll the D1: ATR image for a file named `done` and stop the run when it appears.
if [ "$exit_on_done" -eq 1 ]; then
###	( until atr ./fuse_d1.atr ls -al | grep ' done ' > /dev/null; do sleep 1; done; sleep 1; killall atari800; killall dstitch_mount ) &
( sleep 3; killall atari800; killall dstitch_mount ) & 
fi
mount_sector_args=()
if [ -n "$sdfs_sectors" ]; then
	mount_sector_args=(--sdfs-sectors "$sdfs_sectors")
fi
(
	sleep 1
	if [ "$headless" -eq 1 ]; then
		ATARI800_HEADLESS=1 xvfb-run -a ./atari800.sh
	else
		./atari800.sh
	fi
) > ./atari800.log 2>&1 &
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./libsmb2/build/lib ./dstitch_mount --sdfs "${mount_sector_args[@]}" "$SMB_URL" -f ./mnt
cat atari800.log
exit 0

atr ./fuse_d1.atr get -l foo2 
atr ./fuse_d1.atr get -l bar2 
atr ./fuse_d1.atr get -l done
ls -ltr foo bar foo2 bar2 done
cmp foo done
cmp foo foo2
cmp bar bar2
killall run.sh
