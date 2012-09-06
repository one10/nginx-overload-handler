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
# ==== install general dependencies (for which we do not need source) ====
#
# USAGE: sudo ./install_dependencies.sh
#
# Assumes you're running Ubuntu
#

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source $DIR/env.sh
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

apt-get update

debconf-set-selections <<< "mysql-server-5.1 mysql-server/root_password password $MYSQL_PASSWORD"
debconf-set-selections <<< "mysql-server-5.1 mysql-server/root_password_again password $MYSQL_PASSWORD"

apt-get install -y \
    curl \
    make

# PHP dependencies
apt-get install -y \
    mysql-server-5.1 \
    libxml2-dev \
    mysql-client \
    libmysqlclient15-dev \
    memcached

# memcached is auto-started, if not, do sudo /etc/init.d/memcached start

# The trainer client needs to be able to ssh into the server
apt-get install -y openssh-server

# Needed for Django
apt-get install -y \
    python-setuptools \
    python-pip \
    python-software-properties

# Server benchmarking tools such as ab
apt-get install -y apache2-utils

# misc
apt-get install -y librspec-ruby python-dev
