#!/bin/bash

set -Eeuo pipefail

usage() {
	echo -e \
	"Usage:\n" \
	"  $0 ysu_node [daemon] [cli_options] [-l] [-v size]\n" \
	"    daemon\n" \
	"      start as daemon\n\n" \
	"    cli_options\n" \
	"      ysu_node cli options <see ysu_node --help>\n\n" \
	"    -l\n" \
	"      log to console <use docker logs {container}>\n\n" \
	"    -v<size>\n" \
	"      vacuum database if over size GB on startup\n\n" \
	"  $0 bash [other]\n" \
	"    other\n" \
	"      bash pass through\n" \
	"  $0 [*]\n" \
	"    *\n" \
	"      usage\n\n" \
	"default:\n" \
	"  $0 ysu_node daemon -l"
}

OPTIND=1
command=()
IFS=' ' read -r -a TEMP_OPTS <<<"$@"
passthrough=()
db_size=0
log_to_cerr=0

if [ ${#TEMP_OPTS[@]} -lt 2 ]; then
	usage
	exit 1
fi

if [[ "${TEMP_OPTS[0]}" = 'ysu_node' ]]; then
	unset 'TEMP_OPTS[0]'
	command+=("ysu_node")
	shift;
	for i in "${TEMP_OPTS[@]}"; do
		case $i in
			"daemon" )
				command+=("--daemon")
				;;
			* )
				passthrough+=("$i")
				;;
		esac
	done
	for i in "${passthrough[@]}"; do
		if [[ "$i" =~ ^"-v" ]]; then
		        db_size=${i//-v/}
			echo "Vacuum DB if over $db_size GB on startup"
		elif [[ "$i" = '-l' ]]; then
			echo "log_to_cerr = true"
			command+=("--config")
			command+=("node.logging.log_to_cerr=true")
		else
		 	command+=("$i")
		fi
	done
elif [[ "${TEMP_OPTS[0]}" = 'bash' ]]; then
	unset 'TEMP_OPTS[0]'
	echo -e "EXECUTING ${TEMP_OPTS[*]}\n"
	exec "${TEMP_OPTS[@]}"
	exit 0;
else
	usage
	exit 1;
fi

network="$(cat /etc/ysu-network)"
case "${network}" in
	live|'')
	network='live'
	dirSuffix=''
	;;
	beta)
	dirSuffix='Beta'
	;;
	dev)
	dirSuffix='Dev'
	;;
	test)
	dirSuffix='Test'
	;;
esac

raidir="${HOME}/RaiBlocks${dirSuffix}"
ysudir="${HOME}/Ysu${dirSuffix}"
dbFile="${ysudir}/data.ldb"

if [ -d "${raidir}" ]; then
	echo "Moving ${raidir} to ${ysudir}"
	mv "$raidir" "$ysudir"
else
	mkdir -p "${ysudir}"
fi

if [ ! -f "${ysudir}/config-node.toml" ] && [ ! -f "${ysudir}/config.json" ]; then
	echo "Config file not found, adding default."
	cp "/usr/share/ysu/config/config-node.toml" "${ysudir}/config-node.toml"
	cp "/usr/share/ysu/config/config-rpc.toml" "${ysudir}/config-rpc.toml"
fi

if [[ "${command[1]}" = "--daemon" ]]; then
	if [[ $db_size -ne 0 ]]; then
		if [ -f "${dbFile}" ]; then
			dbFileSize="$(stat -c %s "${dbFile}" 2>/dev/null)"
			if [ "${dbFileSize}" -gt $((1024 * 1024 * 1024 * db_size)) ]; then
				echo "ERROR: Database size grew above ${db_size}GB (size = ${dbFileSize})" >&2
				ysu_node --vacuum
			fi
		fi
	fi
fi
echo -e "EXECUTING: ${command[*]}\n"
exec "${command[@]}"
