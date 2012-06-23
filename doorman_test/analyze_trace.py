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
# ==== analyze_trace.py ====
# analyzes trace files generated by doorman_legit.py and doorman_attack.py
#
# USAGE: cat doorman_legit.py.csv | ./analyze_trace.py
#
# Example output (with explanation):
# cat doorman_attack.py.csv | ./analyze_trace.py
'''
{
    # Latency distributions, grouped by event. There are 4 types of events
    #   * give-puzzle: a web response from the server delivering a puzzle
    #   * web-app: a web response from the server delivering an application page
    #   * solve-puzzle: the client solving a puzzle
    #   * None: a non-valid web response (e.g. 502 or a 404)

    "by_event": {

        # Note: this example output does not have any web-app events because
        # this is output for an attack trace, where each request reults in
        # an infinite loop. Therefore there should be no web-app results

        "None": [
            "quantile 0.250000 -> 0.002217",
            "quantile 0.500000 -> 0.004725",
            "quantile 0.750000 -> 0.014039",
            "quantile 0.950000 -> 0.072224",
            "quantile 1.000000 -> 0.342244",
            "18.89% of 5400 events"           # Invalid web responses accounted for 18.89%
                                              # of all events
        ],
        "give-puzzle": [
            "quantile 0.250000 -> 0.001561",
            "quantile 0.500000 -> 0.002587",
            "quantile 0.750000 -> 0.004578",
            "quantile 0.950000 -> 0.014290",  # the doorman delivered 95% of puzzles to
            "quantile 1.000000 -> 0.072873"   # the attacker within 0.014290 seconds
            "68.54% of 5400 events"
        ],
        "solve-puzzle": [
            "quantile 0.250000 -> 0.000013",
            "quantile 0.500000 -> 0.000014",
            "quantile 0.750000 -> 0.000017",  # the attacker solved 75% of puzzles within
            "quantile 0.950000 -> 0.603872",  # 0.000017 seconds
            "quantile 1.000000 -> 5.019173"   # the attacker solved 100% of puzzles within 5.1 sec
            "12.57% of 5400 events"
        ]
    },
    "by_status": {
        "200": [
            "quantile 0.250000 -> 0.001561",
            "quantile 0.500000 -> 0.002587",
            "quantile 0.750000 -> 0.004578",
            "quantile 0.950000 -> 0.014290",
            "quantile 1.000000 -> 0.072873",
            "68.54% of 5400 events"
        ],
        "404": [
            "quantile 0.250000 -> 0.004521",
            "quantile 0.500000 -> 0.009950",
            "quantile 0.750000 -> 0.028923",
            "quantile 0.950000 -> 0.078865",
            "quantile 1.000000 -> 0.342244",
            "8.39% of 5400 events"
        ],
        "502": [
            "quantile 0.250000 -> 0.001623",
            "quantile 0.500000 -> 0.002906",
            "quantile 0.750000 -> 0.005930",
            "quantile 0.950000 -> 0.046803",
            "quantile 1.000000 -> 0.284697",
            "10.50% of 5400 events"
        ],
        "None": [
            "quantile 0.250000 -> 0.000013",
            "quantile 0.500000 -> 0.000014",
            "quantile 0.750000 -> 0.000017",
            "quantile 0.950000 -> 0.603872",
            "quantile 1.000000 -> 5.019173",
            "12.57% of 5400 events"
        ]
    },
    "num_bits": {
        "num puzzles with 1 bits": 615,
        "num puzzles with 2 bits": 380,
        "num puzzles with 3 bits": 1427,
        "num puzzles with 4 bits": 552,
        "num puzzles with 5 bits": 458,
        "num puzzles with 6 bits": 269
    }
    "success_rate": 0.0                    # == web_app / (web_app + invalid)
}
'''
# Here's the analysis of the legit clients from the same test.
# cat doorman_legit.py.csv | ./analyze_trace.py
'''
{
    "by_event": {
        "None": [
            "quantile 0.250000 -> 0.001792",
            "quantile 0.500000 -> 0.003347",
            "quantile 0.750000 -> 0.268089",
            "quantile 0.950000 -> 3.058224",
            "quantile 1.000000 -> 4.247941",
            "15.82% of 1049 events"
        ],
        "give-puzzle": [
            "quantile 0.250000 -> 0.001221",
            "quantile 0.500000 -> 0.001276",
            "quantile 0.750000 -> 0.001429",
            "quantile 0.950000 -> 0.002565",
            "quantile 1.000000 -> 0.006827",
            "30.98% of 1049 events"
        ],
        "solve-puzzle": [
            "quantile 0.250000 -> 0.200269",
            "quantile 0.500000 -> 0.413833",
            "quantile 0.750000 -> 1.405061",
            "quantile 0.950000 -> 3.214388",
            "quantile 1.000000 -> 6.428887",
            "30.03% of 1049 events"
        ],
        "web-app": [
            "quantile 0.250000 -> 0.983933",
            "quantile 0.500000 -> 1.413846",
            "quantile 0.750000 -> 1.747301",
            "quantile 0.950000 -> 3.247246",
            "quantile 1.000000 -> 8.777762",
            "23.16% of 1049 events"
        ]
    },
    "by_status": {
        "200": [
            "quantile 0.250000 -> 0.001262",
            "quantile 0.500000 -> 0.001831",
            "quantile 0.750000 -> 1.369674",
            "quantile 0.950000 -> 2.183716",
            "quantile 1.000000 -> 8.777762",
            "54.15% of 1049 events"
        ],
        "502": [
            "quantile 0.250000 -> 0.001792",
            "quantile 0.500000 -> 0.003347",
            "quantile 0.750000 -> 0.268089",
            "quantile 0.950000 -> 3.058224",
            "quantile 1.000000 -> 4.247941",
            "15.82% of 1049 events"
        ],
        "None": [
            "quantile 0.250000 -> 0.200269",
            "quantile 0.500000 -> 0.413833",
            "quantile 0.750000 -> 1.405061",
            "quantile 0.950000 -> 3.214388",
            "quantile 1.000000 -> 6.428887",
            "30.03% of 1049 events"
        ]
    },
    "num_bits": {
        "num puzzles with 1 bits": 54,
        "num puzzles with 2 bits": 64,
        "num puzzles with 3 bits": 131,
        "num puzzles with 4 bits": 40,
        "num puzzles with 5 bits": 24,
        "num puzzles with 6 bits": 12
    },
    "success_rate": 0.5941320293398533          # == web_app / (web_app + invalid)
}
'''

import sys
import json
from itertools import groupby

quantiles = [0.25, 0.50, 0.75, 0.95, 1.0]


def quantile(values, q):
    '''assumes values is sorted numbers (and each is >= 0), and 0.0 < q <= 1.0.
    returns x, where at least q * 100 percent of numbers (from values) are <= x'''
    index = int(q * len(values)) - 1
    if index >= 0:
        return values[index]
    else:
        return float("nan")

def analyze(f):
    by_event_latencies = {}
    by_status_latencies = {}
    by_event = {}
    by_status = {}
    num_bits = []

    num_events = 0
    min_timestamp = None
    max_timestamp = None

    for line in sys.stdin:
        parts = line.strip().split(",")
        assert len(parts) == 5, parts
        num_events += 1
        status, event, timestamp, latency, bits = parts
        timestamp = float(timestamp)
        if min_timestamp == None:
            assert(max_timestamp == None)
            min_timestamp = max_timestamp = timestamp
        min_timestamp = min(min_timestamp, timestamp)
        max_timestamp = max(max_timestamp, timestamp)

        if event == "give-puzzle":
            num_bits.append(int(bits))
        if status not in by_status_latencies:
            by_status_latencies[status] = []
        if event not in by_event_latencies:
            by_event_latencies[event] = []
        by_status_latencies[status].append(float(latency))
        by_event_latencies[event].append(float(latency))

    for event, latencies in by_event_latencies.items():
        latencies.sort()
        by_event[event] = ["quantile %f -> %f" % (q, quantile(latencies, q)) for q in quantiles]
        proportion = float(len(latencies)) / float(num_events) * 100.0
        by_event[event].append("%.2f%% of %d events" % (proportion, num_events))
    for status, latencies in by_status_latencies.items():
        latencies.sort()
        by_status[status] = ["quantile %f -> %f" % (q, quantile(latencies, q)) for q in quantiles]
        proportion = float(len(latencies)) / float(num_events) * 100.0
        by_status[status].append("%.2f%% of %d events" % (proportion, num_events))

    # convert num_bits to a histogram
    num_bits.sort()
    num_bits = dict([("num puzzles with %d bits" % item, len(list(group))) for item, group in groupby(num_bits)])

    success = float(len(by_event_latencies.get("web-app", [])))
    fail = float(len(by_event_latencies.get("None", []))) + float(len(by_event_latencies.get("solve-puzzle-timeout", [])))

    success_rate = success / (success + fail)
    time_elapsed = max_timestamp - min_timestamp

    return {
        "success_rate" : success_rate,
        "throughput" : success / time_elapsed,
        "time_elapsed" : time_elapsed,
        "by_event" : by_event,
        "by_status" : by_status,
        "num_bits" : num_bits
    }

print json.dumps(analyze(sys.stdin), sort_keys=True, indent=4)


