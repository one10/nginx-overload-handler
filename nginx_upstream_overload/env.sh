#!/usr/bin/env bash
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
# ==== env.sh ====
#
# defines some shell variables
#

# $DIR is the absolute path for the directory containing this bash script
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

OVERLOAD_MODULE_DIR=$DIR
NGINX_USER=nginx_user
NGINX_GROUP=$NGINX_USER
NGINX_HOME_DIR=/home/$NGINX_USER
NGINX_RAMDISK_DIR=$NGINX_HOME_DIR/ramdisk

# the upstream_overload module will send alerts to this pipe
ALERT_PIPE_PATH=$NGINX_HOME_DIR/alert_pipe
SIG_FILE_PATH=$NGINX_RAMDISK_DIR/signature.txt

