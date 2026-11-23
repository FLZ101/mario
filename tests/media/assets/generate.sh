#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 ]]; then
    printf 'Usage: %s <trailer_480p.mov> <sintel_trailer-480p.mp4>\n' "$0" >&2
    exit 2
fi

output=$(cd -- "$(dirname -- "$0")" && pwd)
ffmpeg=${FFMPEG:-ffmpeg}
video=(-c:v libx264 -profile:v baseline -level:v 3.0 -pix_fmt yuv420p -g 15 -crf 23)
audio=(-c:a aac -profile:a aac_low -ac 2 -b:a 96k)
scaling='fps=15,scale=320:180:force_original_aspect_ratio=decrease:force_divisible_by=2,pad=320:180:(ow-iw)/2:(oh-ih)/2,setsar=1'

make_demo() {
    local source=$1 start=$2 title=$3 credit=$4 filename=$5
    "$ffmpeg" -hide_banner -loglevel error -y -ss "$start" -i "$source" -t 12 \
        -map 0:v:0 -map 0:a:0 -map_metadata -1 -vf "$scaling" \
        "${video[@]}" "${audio[@]}" -ar 48000 -movflags +faststart \
        -metadata title="$title" -metadata copyright="$credit" \
        "$output/$filename"
}

make_demo "$1" 10 'Big Buck Bunny (trailer excerpt)' \
    '(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org; CC BY 3.0' big-buck-bunny.mp4
make_demo "$2" 10 'Sintel (trailer excerpt)' \
    '(c) copyright Blender Foundation | durian.blender.org; CC BY 3.0' sintel.mp4

left='0.35*sin(2*PI*440*t)*(lt(t,0.5)+gte(t,1.5))'
right='0.35*sin(2*PI*660*t)*(gte(t,0.5)*lt(t,1)+gte(t,1.5))'
"$ffmpeg" -hide_banner -loglevel error -y -f lavfi \
    -i "aevalsrc='$left|$right':s=48000:d=2:c=stereo" \
    -c:a pcm_s16le "$output/stereo-markers.wav"

for rate in 44100 48000; do
    "$ffmpeg" -hide_banner -loglevel error -y -i "$output/stereo-markers.wav" \
        "${audio[@]}" -ar "$rate" -movflags +faststart "$output/stereo-markers-$rate.mp4"
done

"$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=black:s=320x180:r=15:d=4,drawbox=c=white:t=fill:enable='gte(n,15)*lt(mod(n,15),3)'" \
    -f lavfi -i "aevalsrc='0.4*sin(2*PI*880*t)*gte(t,1)*lt(mod(t,1),0.2)':s=48000:d=4" \
    "${video[@]}" "${audio[@]}" -ar 48000 -movflags +faststart "$output/flash-beep.mp4"
