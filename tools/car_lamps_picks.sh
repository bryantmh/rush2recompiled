#!/bin/sh
# Car Lights' hand entries (assets/car_lamps.json), each measured with tools/car_lamps.py pick: rerun it after a new
# sweep to measure them again (docs/graphics_effects.md#reviewing-the-lamps). Its inputs:
#   python tools/car_lamps.py sweep tmp/car_lamps_all                       (Rush 2, 2049, Cal Speed, mod cars)
#   python tools/car_lamps.py sweep tmp/car_lamps_dc --types 23-35 --dreamcast
#   python tools/car_lamps.py glb <livery>.glb tmp/liv_glb/<livery>.json --key rush2049:<car>/<livery>
#     (each body glb of build/mods/rush2049_arcade_cars.rtz's rush2/liveries/: their full-size textures)
# The table's other entries (rush2:18's headlamps, the arcade Deuce's tail lamps: lamps with no texels to measure)
# were written by hand.
D=${1:-tmp/car_lamps_all}
P="python tools/car_lamps.py pick $D"
S="--save assets/car_lamps.json"
RED="--color 255,0,0 --tolerance 120"
$P rush2:2 head --x 1.5 1.9 --y 1.25 1.45 --rel 0 --size 0.3 --no-lens $S
$P rush2:6 head --x 1.1 2.4 --y 1.2 2.0 $S
$P rush2:7 head --x 0.6 1.7 --y 0.6 1.2 $S
$P rush2:9 head --x 1.2 2.5 --y 1.25 1.7 --size 0.4 $S
$P rush2:14 head --x 1.2 2.2 --y 1.0 1.6 --size 0.35 $S
$P rush2:16 head --x 1.9 2.7 --y 0.9 1.7 --neutral $S
$P rush2:16 tail --x 2.2 3.0 --y 1.1 2.2 $RED $S
$P rush2:17 head --x 1.6 2.6 --y 0.3 0.8 $S
$P rush2:18 tail --x -0.4 0.4 --y 0.5 1.3 $RED --single $S
$P rush2:19 tail --x 0.3 1.0 --y 0.3 0.8 $RED --size 0.15 $S
$P rush2:20 head --x 0.9 1.6 --y 0.3 0.9 --rel 0 --no-lens --size 0.25 $S
$P rush2:21 head --x 1.2 2.0 --y 1.2 1.8 --rel 0 --no-lens --size 0.2 $S
$P rush2:21 tail --x -0.4 0.4 --y 0.8 1.6 $RED --single $S
$P rush2049:3 head --x 1.6 2.5 --y 0.3 1.1 --neutral $S
$P rush2049:4 head --x 0.7 1.9 --y 1.1 1.7 --neutral $S
$P rush2049:5 head --x 1.8 2.6 --y 0.9 1.4 --neutral $S
$P rush2049:6 head --x 1.3 2.3 --y 0.7 1.2 --neutral $S
$P rush2049:10 head --x 1.4 2.8 --y 1.9 2.5 --neutral --size 0.45 $S
$P rush2049:10 tail --x 2.7 3.4 --y 2.5 3.6 $RED $S
$P rush2049:10 tail --x -0.5 0.5 --y 2.2 2.6 $RED --single --add $S
$P rush2049:11 head --x 1.4 2.4 --y 1.0 1.5 --neutral --size 0.4 $S
$P rush2049:12 head --x 1.4 2.4 --y 1.3 1.7 --neutral $S
$P rush2049:0 head --x 1.3 1.9 --y 0.45 0.95 --color 40,40,170 --tolerance 120 --facing 0.05 --size 0.3 $S
$P rush2049:2 head --x 0.9 1.9 --y 0.6 1.15 --neutral --facing 0.05 $S
$P calspeed:0 tail --x 1.6 2.6 --y 0.9 1.7 $RED $S
$P calspeed:0 tail --x -0.9 0.9 --y 0.9 1.7 $RED --single --add $S
$P calspeed:2 head --x 0.5 1.4 --y 0.3 0.8 --neutral $S
$P calspeed:3 head --x 1.0 1.7 --y 0.1 0.8 --neutral $S
$P calspeed:4 head --x 1.6 2.6 --y 0.3 1.3 --neutral --size 0.45 $S
$P calspeed:6 head --x 1.9 2.7 --y 1.6 2.5 --neutral $S
$P calspeed:6 tail --x 2.2 3.0 --y 1.9 2.7 $RED --facing 0.05 $S
$P calspeed:7 head --x 0.6 1.5 --y 1.1 1.45 $S
$P calspeed:11 head --x 0.6 1.3 --y 1.1 1.7 --rel 0 --no-lens --size 0.25 $S
$P calspeed:11 tail --x 0.6 1.3 --y 1.0 1.6 --rel 0 --no-lens --size 0.25 $S
$P calspeed:15 head --x 1.9 2.7 --y 0.95 1.45 --neutral $S
$P calspeed:16 head --x 1.8 2.6 --y 0.95 1.4 --neutral --size 0.22 $S
$P calspeed:17 head --x 1.8 2.6 --y 1.3 1.75 --size 0.25 $S
$P calspeed:18 head --x 1.0 1.9 --y 1.2 1.9 --neutral --size 0.3 $S
$P calspeed:19 head --x 1.8 2.6 --y 1.0 2.0 --rel 0 --z 5 7 --no-lens --size 0.3 $S
$P calspeed:19 tail --x 2.0 2.9 --y 0.8 1.6 --rel 0 --z -8 -6.5 --no-lens --size 0.3 $S
$P "mod:CADDY" head --x 2.0 2.9 --y 0.6 1.2 --neutral $S
$P "mod:JEEP" tail --x 1.8 2.7 --y 1.5 2.0 $S
$P "mod:JET CAR" tail --x 1.5 2.4 --y 1.4 1.9 $S
$P "mod:MINI" head --x 1.5 2.4 --y 0.5 1.3 --neutral --size 0.4 $S
$P "mod:MINI" tail --x 1.7 2.5 --y 0.4 1.2 --color 200,60,30 --tolerance 100 $S
$P "mod:POLICE" tail --x 1.6 2.8 --y 1.0 1.5 --size 0.4 $S
$P "mod:STARSKY" tail --x 1.7 2.7 --y 1.1 1.7 --color 220,100,30 --tolerance 90 $S
# Liveries, measured on their glbs' full-size textures (tools/car_lamps.py glb into tmp/liv_glb).
L="python tools/car_lamps.py pick tmp/liv_glb"
BLUE="--color 60,130,230 --tolerance 120"
for c in COL4 COL6; do
  $L "rush2049:3/MAGNUM_$c" head --x 1.5 2.5 --y 0.55 1.05 $BLUE $S
  $L "rush2049:3/MAGNUM_$c" head --x 1.5 2.5 --y 0.0 0.55 $BLUE --add $S
done
$L "rush2049:3/MAGNUM_COL4" tail --x 1.3 1.95 --y 1.3 2.1 --color 200,40,40 --tolerance 110 $S
$L "rush2049:3/MAGNUM_COL4" tail --x 1.95 2.6 --y 1.3 2.1 --color 200,40,40 --tolerance 110 --add $S
$L "rush2049:3/MAGNUM_COL7" head --x 1.5 2.5 --y 0.3 1.1 --color 130,220,60 --tolerance 110 $S
$L "rush2049:3/MAGNUM_COL7" tail --x 1.2 2.5 --y 1.4 2.1 --color 220,40,40 --tolerance 110 $S
for c in COL1 COL3; do
  $L "rush2049:1/8BALL_$c" tail --x 1.4 2.7 --y 1.5 2.0 --color 220,70,30 --tolerance 100 $S
  $L "rush2049:1/8BALL_$c" head --x -2.3 -1.3 --y 1.15 1.7 --color 60,100,100 --tolerance 60 --mirror $S
done
$L "rush2049:7/GX2_COL6" head --x 1.4 2.3 --y 0.9 1.4 --color 40,70,110 --tolerance 70 $S
$L "rush2049:7/GX2_COL6" tail --x 1.4 2.7 --y 1.6 2.0 --color 200,50,40 --tolerance 100 $S
for c in COL5 COL7; do
  $L "rush2049:4/SUPERGT_$c" head --x 1.2 2.3 --y 0.8 1.1 --neutral $S
  $L "rush2049:4/SUPERGT_$c" tail --x 1.5 2.6 --y 1.3 1.75 --color 200,50,40 --tolerance 100 $S
done
for c in COL1 COL2 COL3 COL5 COL6 COL7 COL8; do
  Y="1.2 1.9"; [ $c = COL5 ] && Y="1.3 1.68"
  $L "rush2049:5/BRUISER_$c" head --x 1.4 1.95 --y $Y --neutral --size 0.22 $S
  $L "rush2049:5/BRUISER_$c" head --x 1.95 2.6 --y $Y --neutral --size 0.22 --add $S
  $L "rush2049:5/BRUISER_$c" tail --x 1.3 1.85 --y 1.4 2.2 --color 190,40,40 --tolerance 100 --size 0.22 $S
  $L "rush2049:5/BRUISER_$c" tail --x 1.85 2.45 --y 1.4 2.2 --color 190,40,40 --tolerance 100 --size 0.22 --add $S
done
# Dreamcast cars (sweep --dreamcast into tmp/car_lamps_dc).
C="python tools/car_lamps.py pick tmp/car_lamps_dc"
$C rush2049dc:0 head --x 1.4 2.4 --y 1.85 2.3 --neutral --rel 0.7 $S
$C rush2049dc:1 head --x 1.4 2.3 --y 1.3 1.7 --color 70,80,90 --tolerance 50 --size 0.2 $S
$C rush2049dc:3 head --x 1.8 2.6 --y 0.4 1.0 --color 160,200,60 --tolerance 90 $S
$C rush2049dc:5 head --x 1.8 2.6 --y 0.9 1.4 --neutral --size 0.35 $S
$C rush2049dc:8 head --x 1.4 2.3 --y 1.15 1.55 --neutral --size 0.25 $S
$C rush2049dc:9 tail --x 1.6 2.8 --y 1.9 2.4 --color 220,40,30 --tolerance 110 --size 0.4 $S
$C rush2049dc:10 head --x 1.4 2.8 --y 1.9 2.5 --neutral --size 0.45 $S
$C rush2049dc:11 head --x 1.4 2.4 --y 1.0 1.5 --neutral --size 0.4 $S
$C rush2049dc:12 head --x 1.4 2.4 --y 1.2 1.6 --neutral $S
