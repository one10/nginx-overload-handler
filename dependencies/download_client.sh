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
# ==== download_client.sh downloads 3rd party software dependencies ====
#

# $DIR is the absolute path for the directory containing this bash script
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source $DIR/env.sh
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

cd $DOWNLOAD_DIR

wget --output-document=$HTTPERF_DL_LOCAL_PATH $HTTPERF_DL_REMOTE_PATH
tar -xvf $HTTPERF_DL_LOCAL_PATH

