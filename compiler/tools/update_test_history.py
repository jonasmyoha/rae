#!/usr/bin/env python3
import os
import json
import subprocess
from datetime import datetime

STATS_FILE = "stats/test_history.json"
TESTS_DIR = "tests/cases"

def get_git_creation_date(filepath):
    try:
        # Use git to find the first commit that included this file
        date_str = subprocess.check_output(
            ["git", "log", "--diff-filter=A", "--follow", "--format=%aI", "--", filepath]
        ).decode("utf-8").split('\n')[0]
        if not date_str:
            # Fallback if filter=A doesn't work (e.g. renamed)
            date_str = subprocess.check_output(
                ["git", "log", "--reverse", "--format=%aI", "--", filepath]
            ).decode("utf-8").split('\n')[0]
        return date_str
    except Exception:
        return datetime.now().isoformat()

def load_history():
    if os.path.exists(STATS_FILE):
        with open(STATS_FILE, "r") as f:
            return json.load(f)
    return {}

def save_history(history):
    with open(STATS_FILE, "w") as f:
        json.dump(history, f, indent=2)

def update_history(passed_tests):
    history = load_history()
    now = datetime.now().isoformat()
    
    # Get current list of tests
    current_tests = []
    for f in os.listdir(TESTS_DIR):
        if f.endswith(".rae"):
            current_tests.append(f)
            
    for test in current_tests:
        if test not in history:
            print(f"Tracking new test: {test}")
            history[test] = {
                "added_at": get_git_creation_date(os.path.join(TESTS_DIR, test)),
                "last_passed_at": "1970-01-01T00:00:00"
            }
            
        if test in passed_tests:
            history[test]["last_passed_at"] = now
            
    save_history(history)

if __name__ == "__main__":
    import sys
    # Expects a list of passed test filenames as arguments
    if len(sys.argv) > 1:
        update_history(sys.argv[1:])
    else:
        # Just initialize/scan
        update_history([])
