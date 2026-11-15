#!/bin/sh
# Car Lights' hand entries (assets/car_lamps.json), each measured with tools/car_lamps.py pick: rerun it after a new
# sweep to measure them again (docs/graphics_effects.md#reviewing-the-lamps). Its inputs:
#   python tools/car_lamps.py sweep tmp/car_lamps_all                       (Rush 2, 2049, Cal Speed, mod cars)
#   python tools/car_lamps.py sweep tmp/car_lamps_dc --types 23-35 --dreamcast
#   python tools/car_lamps.py glb <livery>.glb tmp/liv_glb/<livery>.json --key rush2049:<car>/<livery>
#     (each body glb of build/mods/rush2049_arcade_cars.rtz's rush2/liveries/: their full-size textures)
#   python tools/car_lamps.py glb <car>.glb tmp/mod_glb/<car>.json --key "mod:<NAME>"
#     (the PlayStation SF Rush cars' and arcade cars' body glbs, build/mods/sfrush_ps1.rtz and rush2049_arcade_cars.rtz
#     rush2/new_cars/: PS1BUS = CITY BUS, PS1JETCAR, PS1MINI, PS1STARSKY, CADDY_COL1, TRUCK_COL1)
# Cars that drive a race lit by the finder alone change lamps with the paint (white paint, a white bumper or chrome
# pass for headlamp glass: GitHub issue 79), so every car a player reported has an entry here.
# The table's other entries (rush2:18's headlamps, the arcade Deuce's tail lamps: lamps with no texels to measure)
# were written by hand.
D=${1:-tmp/car_lamps_all}
P="python tools/car_lamps.py pick $D"
S="--save assets/car_lamps.json"
RED="--color 255,0,0 --tolerance 120"
$P rush2:0 head --x 1.3 2.1 --y 1.6 2.1 --neutral $S
$P rush2:2 head --x 1.5 1.9 --y 1.25 1.45 --rel 0 --size 0.3 --no-lens $S
$P rush2:4 head --x 1.2 1.65 --y 1.25 1.6 --neutral $S
$P rush2:4 head --x 1.7 2.15 --y 1.25 1.6 --neutral --add $S
$P rush2:5 head --x 1.3 1.8 --y 1.2 1.55 --neutral $S
$P rush2:5 head --x 1.8 2.2 --y 1.2 1.55 --neutral --add $S
# rush2:10: then its -x lamp moved by hand to (-1.94, 1.33): in game the car's left bulb is drawn 0.17 ft inboard and
# 0.08 ft below the mirror of its right one, though the dump's body and texels are symmetric.
$P rush2:10 head --x 1.8 2.6 --y 1.1 1.7 --neutral $S
$P rush2:15 head --x 1.9 2.7 --y 1.85 2.35 --neutral $S
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
$P rush2049:5 head --x 1.55 2.05 --y 1.1 1.55 --neutral $S
$P rush2049:5 head --x 2.05 2.5 --y 1.1 1.55 --neutral --add $S
$P rush2049:8 head --x 0.85 1.75 --y 1.25 1.65 --neutral --min 30 $S
# The GX-2's headlamps (rush2049:7, rush2049dc:7, rush2049:7/GX2_COL6) are written by hand, not picked: each eye
# holds a large round bulb (outer, upper) and a small one (inner, lower), which the dumps' textures are too coarse to
# tell apart. They were measured in game (GitHub issue 79): RUSH2_TEST_LOOK_BACK front shots with Car Lights off for
# the bulbs and with marker lamps at known points for the pixel-to-feet map (126 px/ft across, 109 px/ft up at 4K).
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
# The garbage truck's lamps are the rectangles on the fender fronts beside the grille, not the bumper below them.
$P calspeed:19 head --x 2.0 2.8 --y 1.8 2.4 --rel 0 --z 5 7 --no-lens --size 0.3 $S
$P calspeed:19 tail --x 2.0 2.9 --y 0.8 1.6 --rel 0 --z -8 -6.5 --no-lens --size 0.3 $S
# The bulldozer's round cab lamps beside the grille (the blade corners were the body-shape guess); the far side's box
# sees the blade's edge first, so this side is mirrored.
$P calspeed:20 head --x 1.3 1.75 --y 3.8 4.15 --rel 0 --no-lens --size 0.25 --mirror $S
$P "mod:JEEP" tail --x 1.8 2.7 --y 1.5 2.0 $S
$P "mod:JET CAR" tail --x 1.5 2.4 --y 1.4 1.9 $S
$P "mod:MINI" tail --x 1.7 2.5 --y 0.4 1.2 --color 200,60,30 --tolerance 100 $S
$P "mod:POLICE" tail --x 1.6 2.8 --y 1.0 1.5 --size 0.4 $S
$P "mod:STARSKY" tail --x 1.7 2.7 --y 1.1 1.7 --color 220,100,30 --tolerance 90 $S
# Mod cars' headlamps (and the bus's tail lamps), measured on their glbs' full-size textures (tmp/mod_glb, above).
M="python tools/car_lamps.py pick tmp/mod_glb"
$M "mod:CADDY" head --x 1.9 2.9 --y 1.4 2.1 --neutral $S
$M "mod:CITY BUS" head --x 2.2 3.0 --y 1.4 1.8 --neutral --min 200 $S
$M "mod:CITY BUS" tail --x 0.6 1.8 --y 2.0 2.4 $RED $S
$M "mod:JET CAR" head --x 1.0 1.45 --y 0.95 1.4 --neutral $S
$M "mod:JET CAR" head --x 1.5 1.95 --y 0.95 1.4 --neutral --add $S
$M "mod:JET CAR" head --x 2.0 2.6 --y 0.95 1.4 --neutral --add $S
$M "mod:MINI" head --x 1.6 2.5 --y 1.2 1.75 --neutral $S
$M "mod:STARSKY" head --x 1.55 2.05 --y 0.7 1.2 --neutral $S
$M "mod:STARSKY" head --x 2.1 2.65 --y 0.7 1.2 --neutral --add $S
$M "mod:TRUCK" head --x 0.9 2.45 --y 1.5 1.9 --neutral $S
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
$C rush2049dc:2 head --x 1.0 2.3 --y 0.5 1.2 --neutral --size 0.35 $S
$C rush2049dc:5 head --x 1.6 2.1 --y 1.0 1.45 --neutral $S
$C rush2049dc:5 head --x 2.15 2.7 --y 1.0 1.45 --neutral --add $S
$C rush2049dc:8 head --x 1.4 2.3 --y 1.15 1.55 --neutral --size 0.25 $S
$C rush2049dc:9 tail --x 1.6 2.8 --y 1.9 2.4 --color 220,40,30 --tolerance 110 --size 0.4 $S
$C rush2049dc:10 head --x 1.4 2.8 --y 1.9 2.5 --neutral --size 0.45 $S
$C rush2049dc:11 head --x 1.4 2.4 --y 1.0 1.5 --neutral --size 0.4 $S
$C rush2049dc:12 head --x 1.4 2.4 --y 1.2 1.6 --neutral $S
