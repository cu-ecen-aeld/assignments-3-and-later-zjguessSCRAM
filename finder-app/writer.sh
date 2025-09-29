#!/bin/sh

if [ $# -ne 2 ]; then
	echo "Parameters above were not specified"
	exit 1
fi

writefile=$1
writestr=$2

dirpath=$(dirname "$writefile")

if ! mkdir -p "$dirpath"; then
	echo "Directory $dirpath was not made successfully."
fi

if ! echo "$writestr" > "$writefile"; then
	echo "Error: Failed to write to file $writefile"
	exit 1
fi
exit 0
