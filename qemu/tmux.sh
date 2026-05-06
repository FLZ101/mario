#!/bin/bash

set -e

SESSION_NAME="mario"
QEMU_BIN="qemu-system-i386"

cwd=$(pwd)

usage() {
    echo "Usage: $0 start [qemu_args...]"
    echo "       $0 attach"
    echo "       $0 kill"
    echo ""
    exit 1
}

if [ "$1" == "start" ]; then
    shift 1
    if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
        echo "Error: Session '$SESSION_NAME' exists."
        exit 1
    fi

    tmux new-session -d -s "$SESSION_NAME"

    declare -a ARGS
    ARGS=()

    NUM_SERIALS=4
    for ((i=1; i<=$NUM_SERIALS; i++)); do
        tmux new-window -t "$SESSION_NAME"
        WIN=$((i+1))
        rm -f tmp/unix-$WIN
        ARGS+=(-serial "unix:$cwd/tmp/unix-$WIN,server=on,wait=on")
        tmux select-pane -t "$SESSION_NAME:$WIN.1" -T "serial $((i-1))"
        tmux send-keys -t "$SESSION_NAME:$WIN.1" 'clear' Enter
    done

    tmux select-pane -t "$SESSION_NAME:1.1" -T "con"
    tmux send-keys -t "$SESSION_NAME:1.1" 'clear' Enter

    FULL_QEMU_CMD=( "$QEMU_BIN" "${ARGS[@]}" "$@" )

    tmux send-keys -t "$SESSION_NAME:1.1" "${FULL_QEMU_CMD[*]}" Enter

    for ((i=1; i<=$NUM_SERIALS; i++)); do
        WIN=$((i+1))
        socket=$cwd/tmp/unix-$WIN
        while [[ ! -e $socket ]] ; do
            echo "Waiting for $socket ..."
            sleep 1
        done
        tmux send-keys -t "$SESSION_NAME:$WIN.1" "socat STDIO,cfmakeraw,isig=1 UNIX:$socket" Enter
    done

    tmux select-window -t "$SESSION_NAME:1"

    echo ":-)"

# --- COMMAND: ATTACH ---
elif [ "$1" == "attach" ]; then
    if ! tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
        echo "Session not found."; exit 1;
    fi
    tmux attach -t "$SESSION_NAME"

# --- COMMAND: KILL ---
elif [ "$1" == "kill" ]; then
    if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
        tmux kill-session -t "$SESSION_NAME"
        echo ":-)"
    fi
else
    usage
fi
