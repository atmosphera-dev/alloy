# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Independent cleanup attempts for resources owned by one test fixture."""
import subprocess


def cleanup_steps(*steps):
    errors = []
    for step in steps:
        try:
            step()
        except BaseException as error:
            errors.append(error)
    if errors:
        raise BaseExceptionGroup('Fixture cleanup failed after all attempts', errors)


def stop_process(child):
    if child is None:
        return
    errors = []
    try:
        if child.poll() is None:
            try:
                child.terminate()
            except ProcessLookupError:
                pass
        try:
            child.wait(timeout=30)
        except subprocess.TimeoutExpired as error:
            errors.append(error)
            try:
                child.kill()
            except ProcessLookupError:
                pass
            child.wait(timeout=30)
    except BaseException as error:
        errors.append(error)
    if errors:
        raise BaseExceptionGroup('Owned process required forced cleanup or failed reaping', errors)
