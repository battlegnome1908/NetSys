# WebServer-grading-script

I wrote a grading script for PA1 [grade.py](https://o365coloradoedu-my.sharepoint.com/personal/tutr6272_colorado_edu/_layouts/15/onedrive.aspx?id=%2Fpersonal%2Ftutr6272%5Fcolorado%5Fedu%2FDocuments%2FNetwork%20System%20Grading%20Script%2FPA1%2DWebserver%2Fgrade%2Epy&parent=%2Fpersonal%2Ftutr6272%5Fcolorado%5Fedu%2FDocuments%2FNetwork%20System%20Grading%20Script%2FPA1%2DWebserver&ga=1). The details are as below.

First, you must put the grading script in the same directory with your project and document root directory. 
As mentioned in the writeup, you must use the provided document root directory [www](https://o365coloradoedu-my.sharepoint.com/:u:/g/personal/tutr6272_colorado_edu/EVSxeyS3FIdJuLDx-NhlqxsBGVkTMf50Mdqdt_AHxYohEw?e=XP0hii).
```
grade.py   Makefile   server    server.c   www
```
Then, you can execute the script by
```
python3 grade.py --exe ./server --port 8000 --class-code 5273
```
Here is a short summary of the script. Noted that the grade breakdown below is for CSCI-5273 students.

CSCI-4273 students can use `--class-code 4273` to get the grade breakdown as described in the writeup (with 5 extra points).

1. Build (gate) – 0 pts if fail

  - Runs `make clean && make -j`.

  - If compilation fails → score = 0, grading stops.

2. Launch server – 0 pts if fail

  - Starts `./server 8000` in the background (configurable with --exe and --port).

  - Waits for readiness via curl; if it crashes (SIGSEGV) or never responds → grading stops.

3. index.html exact match – 60 pts

  - Fetches `http://127.0.0.1:<port>/index.html`.

  - Compares byte-for-byte with `www/index.html`.

  - Exact match → +60, otherwise +0 (prints SHA-256 of served vs local for debugging).

4. Images exact match – up to 10 pts

  - Targets: `images/apple_ex.png, images/exam.gif, images/wine3.jpg`.

  - Each fetched from the server and compared byte-for-byte to `www/images/....`

  - Scoring: +10 if all 3 match; –3 per mismatch; if none match → 0.

5. Handling multiple connections (concurrency) – 15 pts

  - Sends a burst of concurrent requests (default 15 requests, 15 workers) to index.html and the 3 images.

  - Success = HTTP 200 and body matches local file.

  - Score = number of successes.

  - If server segfaults during this step → 0 for this step and grading stops.

6. Error handling (three checks) – 5 pts total

  - 404 Not Found (+3): `curl http://127.0.0.1:<port>/images/wine4.jpg` and check body contains “404”(curl|grep semantics).

  - 405 Method Not Allowed (+1): `PUT /index.html` (expects 405).

  - 505 HTTP Version Not Supported (+1): Raw request `GET / HTTP/2.0` (expects 505).

  - Each subtest awards its points independently; prints the observed status/preview if wrong.

7. Persistent connection (keep-alive) – 10 pts

  - One TCP connection; sends three HTTP/1.1 requests in sequence: Keep-Alive → Keep-Alive → Close.

  - Expects three 200 OK responses, each with Content-Length and a body equal to www/index.html.

  - All three must pass on the same connection → +10, otherwise 0.

Max score: 100 pts

(60 index + 10 images + 15 concurrency + 5 errors + 10 keep-alive)

