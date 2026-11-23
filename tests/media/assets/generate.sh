#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 ]]; then
    printf 'Usage: %s <trailer_1080p.mov> <sintel_trailer-480p.mp4>\n' "$0" >&2
    exit 2
fi

output=$(cd -- "$(dirname -- "$0")" && pwd)
ffmpeg=${FFMPEG:-ffmpeg}
video=(-c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 15 -crf 23)
video_sd=("${video[@]}" -level:v 3.0)
audio=(-c:a aac -profile:a aac_low -ac 2 -b:a 96k)

make_demo() {
    local source=$1 start=$2 title=$3 credit=$4 filename=$5
    local width=${6:-320} height=${7:-180} level=${8:-3.0}
    local scaling="fps=15,scale=$width:$height:force_original_aspect_ratio=decrease:force_divisible_by=2,pad=$width:$height:(ow-iw)/2:(oh-ih)/2,setsar=1"
    "$ffmpeg" -hide_banner -loglevel error -y -ss "$start" -i "$source" -t 12 \
        -map 0:v:0 -map 0:a:0 -map_metadata -1 -vf "$scaling" \
        "${video[@]}" -level:v "$level" "${audio[@]}" -ar 48000 -movflags +faststart \
        -metadata title="$title" -metadata copyright="$credit" \
        "$output/$filename"
}

bunny_title='Big Buck Bunny (trailer excerpt)'
bunny_credit='(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org; CC BY 3.0'
make_demo "$1" 10 "$bunny_title" "$bunny_credit" big-buck-bunny.mp4
make_demo "$1" 10 "$bunny_title" "$bunny_credit" big-buck-bunny-720p.mp4 1280 720 3.1
make_demo "$1" 10 "$bunny_title" "$bunny_credit" big-buck-bunny-1080p.mp4 1920 1080 4.0
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
    "${video_sd[@]}" "${audio[@]}" -ar 48000 -movflags +faststart "$output/flash-beep.mp4"

drift='testsrc2=s=320x180:r=15:d=32,drawbox=x=32:y=60:w=64:h=64:c=black:t=fill'
drift+=",drawbox=x=32:y=60:w=64:h=64:c=white:t=fill:enable='gte(n,15)*lt(mod(n,15),3)'"
drift+=',drawbox=x=96:y=60:w=16:h=64:c=white:t=fill'
drift+=',drawbox=x=112:y=60:w=144:h=64:c=black:t=fill'
for ((bit = 0; bit < 9; bit++)); do
    drift+=",drawbox=x=$((112 + bit * 16)):y=60:w=16:h=64:c=white:t=fill:enable='mod(floor(n/$((1 << bit))),2)'"
done
"$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i "$drift" \
    -f lavfi -i "aevalsrc='0.4*sin(2*PI*880*t)*gte(t,1)*lt(mod(t,1),0.2)':s=48000:d=32" \
    "${video_sd[@]}" "${audio[@]}" -ar 48000 -movflags +faststart "$output/flash-beep-drift.mp4"

"$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=blue:s=320x180:r=15:d=8,drawbox=c=black:t=fill:enable='lt(t,1)',drawbox=c=red:t=fill:enable='gte(t,1)*lt(t,5)'" \
    "${video_sd[@]}" -movflags +faststart "$output/video-markers.mp4"

hd='testsrc2=s=320x180:r=30,pad=1920:1080:800:810,drawbox=c=blue:h=ih*2/3:t=fill'
hd+=",drawbox=c=red:h=ih*2/3:t=fill:enable='gte(n,6)'"
for ((bit = 0; bit < 4; bit++)); do
    cell="x=$((576 + bit * 192)):y=384:w=128:h=192"
    hd+=",drawbox=$cell:c=black:t=fill"
    hd+=",drawbox=$cell:c=white:t=fill:enable='mod(floor(n/$((1 << bit))),2)'"
done
"$ffmpeg" -hide_banner -loglevel error -y -f lavfi -i "$hd" \
    -c:v libx264 -profile:v baseline -level:v 4.0 -pix_fmt yuv420p -g 30 -crf 26 \
    -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m \
    -frames:v 12 -movflags +faststart "$output/video-markers-1080p.mp4"
