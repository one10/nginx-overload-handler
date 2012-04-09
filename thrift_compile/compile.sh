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
# ==== configures and compiles thrift ====
#

CWD=`pwd`

THRIFT_SOURCE_ORIG=../thrift-0.8.0/*
COMPILE_DIR=$CWD/compiled_thrift
PYTHON_LIB_INSTALL=$CWD/python_thrift_lib

mkdir -p $COMPILE_DIR
mkdir -p $PYTHON_LIB_INSTALL

cp -r $THRIFT_SOURCE_ORIG $COMPILE_DIR

cd $COMPILE_DIR

./configure --with-python PY_PREFIX=$PYTHON_LIB_INSTALL

make
