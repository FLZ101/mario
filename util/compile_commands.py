import fcntl
import json
import os


def record_compile_commands(arguments, source_suffixes):
    database = os.getenv("COMPILE_COMMANDS_JSON")
    if not database:
        return

    sources = [
        argument
        for argument in arguments
        if not argument.startswith("-") and argument.endswith(source_suffixes)
    ]
    if not sources:
        return

    directory = os.getcwd()
    with open(database, "a+", encoding="utf-8") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        stream.seek(0)
        try:
            entries = json.load(stream)
        except json.JSONDecodeError:
            entries = []

        for source in sources:
            for entry in entries:
                if entry["directory"] == directory and entry["file"] == source:
                    entry["arguments"] = arguments
                    break
            else:
                entries.append(
                    {"directory": directory, "arguments": arguments, "file": source}
                )

        stream.seek(0)
        stream.truncate()
        json.dump(entries, stream, indent=2)
