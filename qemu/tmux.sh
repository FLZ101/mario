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

    mkdir -p "$cwd/tmp"

    tmux new-session -d -s "$SESSION_NAME"
    CON_WIN=$(tmux display-message -t "$SESSION_NAME" -p '#{window_index}')

    declare -a ARGS
    ARGS=()
    SERIAL_WINS=()

    NUM_SERIALS=4
    for ((i=0; i<NUM_SERIALS; i++)); do
        WIN=$(tmux new-window -d -P -F '#{window_index}' -t "$SESSION_NAME")
        SERIAL_WINS+=("$WIN")
        rm -f "$cwd/tmp/unix-$WIN"
        ARGS+=(-serial "unix:$cwd/tmp/unix-$WIN,server=on,wait=on")
        tmux select-pane -t "$SESSION_NAME:$WIN" -T "serial $i"
        tmux send-keys -t "$SESSION_NAME:$WIN" 'clear' Enter
    done

    tmux select-pane -t "$SESSION_NAME:$CON_WIN" -T "con"
    tmux send-keys -t "$SESSION_NAME:$CON_WIN" 'clear' Enter

    FULL_QEMU_CMD=( "$QEMU_BIN" "${ARGS[@]}" "$@" )

    tmux send-keys -t "$SESSION_NAME:$CON_WIN" "${FULL_QEMU_CMD[*]}" Enter

    for ((i=0; i<NUM_SERIALS; i++)); do
        WIN=${SERIAL_WINS[$i]}
        socket="$cwd/tmp/unix-$WIN"
        for ((try=1; try<=30; try++)); do
            [ -e "$socket" ] && break
            echo "Waiting for $socket ..."
            sleep 1
        done
        if [ ! -e "$socket" ]; then
            echo "Error: timed out waiting for $socket; is QEMU starting?" >&2
            exit 1
        fi
        tmux send-keys -t "$SESSION_NAME:$WIN" "socat STDIO,cfmakeraw,isig=1 UNIX:$socket" Enter
    done

    tmux select-window -t "$SESSION_NAME:$CON_WIN"

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
