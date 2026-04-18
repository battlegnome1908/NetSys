# PA4 Automated Grading Script

This repository provides an **automated grading script** for the Distributed File System project (**PA4**) in CSCI 4273/5273.  
The grader evaluates the correctness and robustness of your DFS/DFC implementation across multiple server configurations.

---

## 🧩 Overview

The grading script (`grade_pa4.py`) automatically:

1. Builds your code (`make clean && make`)
2. Starts up to **4 DFS servers**
3. Uploads (`put`) and downloads (`get`) test files using your `dfc` client
4. Checks for:
   - File integrity (hash match)
   - Correct distribution of file pieces
   - Redundancy and fault tolerance
   - Proper handling of incomplete file reconstruction
   - Concurrent client support

---

## ⚙️ Prerequisites

- Python ≥ 3.8  
- Ubuntu 20.04 / 22.04 environment  
- `make`, `gcc`, `openssl`, and networking permissions
- Your compiled executables must be named:
  - `dfs`
  - `dfc`
- Ensure `dfc.conf` and `Makefile` are in the same working directory

---

## 🗂 Directory Layout

During grading, the following directories will be created:

```
/home/student/PA4/
├── dfs1/
├── dfs2/
├── dfs3/
├── dfs4/
├── dfc.conf
├── sample_file/
│   ├── wine3.jpg
│   └── apple_ex.png
├── dfc1/
├── dfc2/
├── dfc3/
├── dfc4/
└── grade.py
```

The `sample_file/` directory holds reference files downloaded from the course test server.

---

## 🚀 Usage

Run the grader with **4 port numbers** for your DFS servers:

```bash
python3 grade.py <port_1> <port_2> <port_3> <port_4>

# Example
python3 grade.py 10001 10002 10003 10004
```

The script:
- Cleans and rebuilds your code (`make -s clean && make -s`)
- Kills any processes already bound to the given ports
- Starts servers under:
  ```bash
  ./dfs ./dfs1 10001 &
  ./dfs ./dfs2 10002 &
  ./dfs ./dfs3 10003 &
  ./dfs ./dfs4 10004 &
  ```
- Executes client commands and evaluates output

---

## 🧪 Tests and Scoring

| Test                     | Description                                                                                                        | Points |
|-------------------------|--------------------------------------------------------------------------------------------------------------------|:------:|
| **Test 1: 4 Servers**   | Upload sample files, verify full reconstruction; check piece distribution (4/server = 16 total); run `dfc list`. | **50** |
| **Test 2: 3 Servers**   | Kill server 4; redundancy allows `list` + `get` to succeed; hashes match.                                          | **20** |
| **Test 3: 2 Servers**   | Kill server 2; redundancy still holds; hashes match after fresh `get`.                                             | **20** |
| **Test 4: 1 Server**    | Kill server 3; with one server left, `list` shows "\<filename> [incomplete]" and `get` prints “\<filename> is incomplete”.     | **10** |
| **Test 5: Multiple DFCs** | Restart all 4 servers; run 4 concurrent clients (`dfc1`–`dfc4`) to `get` `wine3.jpg`; all 4 hashes match sample.  | **10** |

**Total:** 110 points

---

## 🧾 Output Example

Refer to `expected_output.txt` file in this repo. 

---

## 🧹 Cleanup

All DFS server processes are safely terminated at the end of the run:

```
[*] Stopping servers ...
```

To manually stop lingering servers:

```bash
pkill -f dfs
```

---

## 🧠 Tips

- Make sure `dfc` reads `dfc.conf` from the **current working directory**.
- Ensure your DFS properly replicates file pieces and handles missing servers gracefully.
- Test your own client using:
  ```bash
  ./dfc put sample_file/wine3.jpg
  ./dfc get wine3.jpg
  ./dfc list
  md5sum wine3.jpg
  md5sum sample_file/wine3.jpg
  ```
- Avoid hardcoding absolute paths—`getcwd()` is used dynamically.

---

