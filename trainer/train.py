#!/usr/bin/env python
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
# ==== train.py ====
#
# USAGE: ./train.py completion period workers username server
#   where:
#       completion is the "minimal completion rate" (a number between 0 and 1)
#       period is the __initial__ interarrival period
#       workers is the numer of fastcgi workers on the server
#       usernaname is a user on server who has permission to restart the
#           the fcgi workers (see restart_remote_fcgi.sh)
#       server is the address of the server (or domain name)
#
# The goal is to run the web application under a variety of Beer Garden
# configurations, and recommend several configurations where at least
# 'completion' portion of legitimate requests complete. The trainer than
# reports the performance metrics from the trials, so you can choose the
# configuration with the "best" performance. "Best" is subjective; there is
# often a trade space between latency, throughput, and completion rate. The
# only setting the trainer modifies is 'period', interarrival time between
# requests.
#
# PREREQs:
#   (1) create a trace file
#
# ==== Design ====
#
# (1) Find M, the "minimal period." I.e., M is the smallest period that Beer
# Garden will recommend.
# (2) Find alternate periods by increasing M by 15% ten times.
# (3) Report the configurations, each configuration has 3 performance metrics
#     associated with its performance: throughput, completion rate, latency. Sort
#     configurations three different ways (according to each metric).
#

#TODO: make sure all file accesses are absolute and refer to the correct parent dir

import json
import subprocess
import urllib2
import argparse
from analyze_trace_output import AnalyzeTraceOutput
import os
import sys
import logging
import time

DIRNAME = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(DIRNAME, '..', 'common'))
RESTART_SCRIPT = os.path.join(DIRNAME, "restart_remote_fcgi.sh")

import log
import env
import restart_remote_fcgi

siteconfig = os.path.join(DIRNAME, "..", "siteconfig.sh")
var = env.env(siteconfig)
SERVER_NAME = var["SERVER_NAME"]

class TrainError(Exception):
    pass

class Train:

    def __init__(self,
            completion_rate,
            intial_period,
            trace_filename,
            username,
            server,
            sshport,
            num_tests,
            test_size,
            logger):
        self.logger = logger
        self.completion_rate = completion_rate
        self.initial_period = intial_period
        self.trace_filename = trace_filename
        self.username = username
        self.server = server
        self.sshport = sshport
        self.num_tests = num_tests
        self.test_size = test_size
        # there should be self.num_requests sessions in self.trace_filename
        # TODO: assert this assumption
        self.num_requests = self.test_size * num_tests
        # TODO: come up with a principled timeout
        self.timeout = 5
        self.results = {}
        self.httperf_stdout_template = "httperf_stdout_%03d.txt"
        self.httperf_stderr_template = "httperf_stderr_%03d.txt"
        self.quantiles = set([0.25, 0.5, 0.75, 1.0, self.completion_rate])

        # Find the url that represents a legitimate request
        with open(self.trace_filename) as f:
            lines = f.readlines()
            page = lines[(self.test_size - 1) * 2].strip()
            self.legit_url = "http://%s%s" % (self.server, page)

    def restart_remote_fcgi(self, trial_num):
        self.logger.info("Restarting FCGI workers on server")
        restart_remote_fcgi.restart_remote_fcgi( \
            self.server, \
            self.username, \
            self.sshport, \
            self.legit_url, \
            self.logger)

    def run_httperf(self, period, trial_num):
        '''Executes httperf, adds the results to self.results, and
        returns the completion rate'''
        self.logger.debug("period=%f, trial_num=%d" % (period, trial_num))
        cmd = ["httperf",
                "--hog",
                "--server=%s" % self.server,
                "--wsesslog=%d,1,%s" % (self.num_requests, self.trace_filename),
                "--period=%f" % period,
                "--timeout=%f" % self.timeout,
                "--print-reply=header",
                "--print-request=header"]

        self.logger.debug(" ".join(cmd))
        #return

        httperf_stdout_filename = self.httperf_stdout_template % trial_num
        #httperf_stderr_filename = self.httperf_stderr_template % trial_num
        with open(httperf_stdout_filename, "w") as stdout:#, \
             #open(httperf_stderr_filename, "w") as stderr:
            p = subprocess.Popen(
                cmd,
                bufsize=1,
                stdout=stdout,
                stderr=subprocess.PIPE)
            stderrLogger = log.FileLoggerThread(self.logger, "httperf stderr", logging.WARNING, p.stderr)
            stderrLogger.start()
            ret = p.wait()
            if ret != 0:
                raise TrainError("httperf for trial %d returned %d" % (trial_num, ret))

        with open(httperf_stdout_filename, "r") as infile:
            analysis = AnalyzeTraceOutput(infile, self.test_size, self.logger)

        self.results[period] = analysis.summary(period, self.quantiles)
        self.logger.debug("results[period=%f] = %s", period, json.dumps(self.results[period], indent=2, sort_keys=True))
        return self.results[period]["completion_rate"]

    def do_trial(self, period):
        self.restart_remote_fcgi(self.trial_num)
        completion_rate = self.run_httperf(period, self.trial_num)
        self.trial_num += 1
        return completion_rate

    def explore_initial_period(self):
        '''Do trials until completion_rate >= self.completion_rate. During each
        new trial, period is multiplied by 2 to get a new period. Returns
        (fail_period, success_period) where success_period is the period with
        the good completion_rate and fail_period was the immediately preceding
        period.'''
        self.logger.info("[Phase 1/3] Phase begin. Exploring different periods until there is a 'successful' trial")
        period = self.initial_period
        self.logger.info("[Phase 1/3] Running first trial with period = %f", period)
        completion_rate = self.do_trial(period)
        self.logger.info("[Phase 1/3] First trial completion rate = %f", completion_rate)
        while completion_rate < self.completion_rate:
            period *= 2.0
            self.logger.info("[Phase 1/3] Trial #%d beginning. Doubling period to %f", self.trial_num, period)
            completion_rate = self.do_trial(period)
            self.logger.info("[Phase 1/3] Trial #%d finished. Completion rate = %f", self.trial_num - 1, completion_rate)
        if period == self.initial_period:
           raise TrainError("[Phase 1/3] Aborting because the first trial succeeded. Re-run trainer with a shorter initial period")

        self.logger.info("[Phase 1/3] Phase finished")
        lower = period / 2.0
        upper = period
        self.logger.info("[Phase 1/3] Period lower bound = %f, upper bound = %f", lower, upper)

        return (lower, upper)

    def find_minimal_period(self, precision=6):
        # Use explore_initial_period to find a pair of periods (fail, success).
        # Then iteratively refine (fail, success) using a binary search (until
        # we reach a desired level of precision).

        fail_period, success_period = self.explore_initial_period()

        self.logger.info("[Phase 2/3] Finding 'minimal period,' i.e. the smallest period with a 'successful' trial")
        for i in range(0, precision):
            trial_period = (fail_period + success_period) / 2.0
            self.logger.info("[Phase 2/3] Trial #%d, testing with period = %f", self.trial_num, trial_period)
            completion_rate = self.do_trial(trial_period)
            self.logger.info("[Phase 2/3] Trial #%d finished. Completion rate = %f", self.trial_num, completion_rate)
            self.logger.debug("(f=%f, try=%f, s=%f] --> %f" % (fail_period, trial_period, success_period, completion_rate))
            if completion_rate < self.completion_rate:
                self.logger.info("[Phase 2/3] Trial #%d was unsuccessful, increasing period next Phase-2 trial", self.trial_num)
                fail_period = trial_period
            else:
                self.logger.info("[Phase 2/3] Trial #%d was successful, decreasing period next Phase-2 trial", self.trial_num)
                success_period = trial_period

        self.logger.info("[Phase 2/3] Phase finished.")
        self.logger.info("[Phase 2/3] minimal period = %f", success_period)
        return success_period

    def explore_alternate_periods(self, period, num_trials=10, increase_rate=1.15):

        completion_rate = self.results[period]["completion_rate"]

        # If the minimal_period leads to a completion rate of 100% then
        # there is not point in exploring alterante configurations, because
        # they should be strictly worse (because throughput will increase
        # while everything else stays the same).
        if completion_rate == 1.0:
            self.logger.info("[Phase 3/3] No need for Phase 3 since minimal period leads to completion rate of 1.0.")
        else:
            self.logger.info("[Phase 3/3] Exploring alternate period bigger than the minimal period. These trials should all be successful.")
            for i in range(0, num_trials):
                period *= increase_rate
                self.logger.info("[Phase 3/3] Trial #%d, testing with period = %f", self.trial_num, period)
                completion_rate = self.do_trial(period)
                self.logger.info("[Phase 3/3] Trial #%d finished. Completion rate = %f", self.trial_num, completion_rate)
                if completion_rate == 1.0:
                    self.logger.info("[Phase 3/3] No need to explore further since, this period leads to completion rate of 1.0.")
                    break
        self.logger.info("[Phase 3/3] Phase finished.")

    def train(self):
        self.trial_num = 1
        minimal_period = self.find_minimal_period()
        self.explore_alternate_periods(minimal_period)
        self.logger.debug(json.dumps(train.results, indent=2, sort_keys=True))

if __name__ == "__main__":
    cwd = os.getcwd()

    default_trace_filename = os.path.join(cwd, "trace.txt")

    parser = argparse.ArgumentParser(description='Trains Beer Garden. See source for more info.')
    parser.add_argument("-c", "--completion", type=float, default=0.95,
                    help="Default=%(default)f. The minimal completion rate you're willing to accept")
    parser.add_argument("-p", "--period", type=float, default=0.01,
                    help="Default=%(default)f. The initial inter-arrival period. Beer Garden should not " \
                    "reach COMPLETION rate when running with PERIOD.")
    parser.add_argument("-t", "--trace", type=str, default=default_trace_filename,
                    help="Default=%(default)s. The trace file (produced by make_trial_trace.py)")
    parser.add_argument("-u", "--username", type=str, default="beergarden",
                    help="Default=%(default)s. The username on the server. Used when "\
                    "invoking restart_remote_fcgi.sh (see its source for PREREQs for username)")
    parser.add_argument("-s", "--server", type=str, default=SERVER_NAME,
                    help="Default=%(default)s. The address of the server running Beer Garden.")
    parser.add_argument("--sshport", type=int, default=22,
                    help="Default=%(default)s. The port of the server listens for ssh.")
    parser.add_argument("-n", "--num-tests", type=int, required=True,
                    help="REQUIRED. The number of tests in the trace file (see --trace and make_trial_trace.py)")
    parser.add_argument("-ts", "--test-size", type=int, required=True,
                    help="REQUIRED. The size of each test in the trace file (see --trace and make_trial_trace.py)")

    log.add_arguments(parser)
    args = parser.parse_args()
    logger = log.getLogger(args)
    logger.info("Command line arguments: %s" % str(args))

    try:
        with open(args.trace, "r") as f:
            pass
    except:
        logger.critical("Error: could not open trace file (%s)" % args.trace)
        sys.exit(1)

    train = Train(
        args.completion,
        args.period,
        args.trace,
        args.username,
        args.server,
        args.sshport,
        args.num_tests,
        args.test_size,
        logger)
    try:
        train.train()
    except TrainError, e:
        self.logger.exception("Aborting training")


