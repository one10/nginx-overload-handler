Copyright 2012 HellaSec, LLC

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

==== README.txt for the dummy FastCGI webapp ====

fcgi_worker_process.p implements a simple FastCGI web application that
is useful for testing upstream_overload module.

==== How to test upstream_overload module (without Bouncer) ====

PREREQUISITE:
    Ensure nginx is compiled and installed with the upstream_overload module.
    See ../nginx_upstream_overload/README.txt

In one terminal:
    ../nginx_upstream_overload/alert_reader.py /home/nginx_user/alert_pipe

In another terminal:
    sudo ./install.sh
    ./launch_fastcgi_workers.sh
    sudo ../nginx_upstream_overload/launch_nginx.sh
    ./send_requests.sh

==== How to test Bouncer with upstream_overload module ====

PREREQUISITE:
(1) Ensure nginx is compiled and installed with the upstream_overload module.
    See ../nginx_upstream_overload/README.txt
(2) Build bouncer dependencies. SEe ../bouncer/README.txt

In separate terminals:
(1) Launch two bouncers (which also launches the FastCGI workers)
    ./bouncer_for_dummy_app.py bouncer_config.json 127.0.0.1 3001
    ./bouncer_for_dummy_app.py bouncer_config.json 127.0.0.1 3002
(2) Launch the Alert Router:
    ../bouncer/alert_router.py bouncer_config.json
(3) Launch nginx
    sudo ../nginx_upstream_overload/launch_nginx.sh
(4) Send a mix of big request and quick requests in rapid succession.
    All the quick requests should be serviced (whereas the bouncer
    kills the big requests). You should see "Oh hai!" appear every
    second (from the quick requests).
    ./send_loop.sh

