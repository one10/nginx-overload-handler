#!usr/bin/env bash
#
# Copyright 2012 HellaSec, LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# ==== overload.sh ====
#
# Overloads a web application by making repeated requests to the dummy
# vulnerablity located at url
#
# USAGE: ./overload.sh url num_requests sleep_time
#

URL=$1
NUM_REQUESTS=$2
SLEEP_TIME=$3

for i in `seq 1 $NUM_REQUESTS`
do
    echo -n "Attack-$i "
    curl -s $URL > /dev/null &
    sleep $SLEEP_TIME
done

echo

