#!/bin/sh
#-z determines if there is a 0 value 
if [ $# -ne 2 ]; then
	echo "Parameters above were not specified"
	exit 1
fi

filesdir=$1
searchstr=$2

#-d determines if a directory exists or not
if [ ! -d "$filesdir" ]; then
	echo "Directory does not exist"
	exit 1
fi
file_count=$(find "$filesdir" -mindepth 1 | wc -l)
match_count=$(grep -r "$searchstr" "$filesdir" | wc -l)
echo "The number of files are $file_count and the number of matching lines are $match_count"
exit 0


