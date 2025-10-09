# Copyright (c) 2025 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

import os
import subprocess
from pathlib import Path

THIRD_DIR = Path("./third")
REPOS = {
    "https://github.com/gflags/gflags.git": "v2.2.2",
    "https://github.com/google/googletest.git": "release-1.11.0",
    "https://github.com/gperftools/gperftools.git": "gperftools-2.12",
    "https://github.com/ebiggers/libdeflate.git": "v1.23",
    "https://github.com/lz4/lz4.git": "v1.9.3",
    "https://github.com/gabime/spdlog.git": "v1.8.2",
    "https://github.com/facebook/zstd.git": "v1.5.5",
    "https://github.com/madler/zlib.git": "v1.3.1",
    "https://github.com/openssl/openssl.git": "OpenSSL_1_0_1-stable",
    "https://github.com/skarupke/flat_hash_map.git": "master"
}

def clone_repo(url, version):
    repo_name = url.split("/")[-1].replace(".git", "")
    dest_dir = THIRD_DIR / repo_name
    
    if dest_dir.exists():
        print(f"Exists: {repo_name}")
        return True
        
    try:
        dest_dir.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", version, url, str(dest_dir)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        print(f"Success: {repo_name}@{version}")
        return True
    except Exception:
        print(f"Error: Failed to clone {repo_name}")
        return False

def main():
    THIRD_DIR.mkdir(parents=True, exist_ok=True)
    
    results = [clone_repo(url, ver) for url, ver in REPOS.items()]
    
    if all(results):
        print("All dependencies fetched")
        return 0
    else:
        print("Some dependencies failed to fetch")
        return 1

if __name__ == "__main__":
    raise SystemExit(main())