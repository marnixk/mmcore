' SFXTEST.BAS - background music plus WAV effects.
' Run with:  RUN "A:/apps/SFXTEST.BAS"
'
' Music keeps playing while 1, 2 and 3 fire effects on top of it. Effects
' never stop, restart or rewind the music and never block this loop, so the
' same pattern works for a game's input, update and draw pass. ESC quits.
'
' The seeded TEST.WAV is a 3 s tone, so holding a key retriggers it; up to
' four effect voices overlap and a fifth trigger replaces the oldest.

OPTION AUDIO_TARGET HDMI
PLAY VOLUME 70
PLAY SFX VOLUME 80
PLAY MODFILE "A:/tests/TEST.MOD"

CLS RGB(0, 0, 0)
COLOUR RGB(220, 220, 220), RGB(0, 0, 0)

DO
  ' A one-shot module stops at its end; start it again so the game always
  ' has music. PLAYING() also proves an effect does not touch the track.
  IF PLAYING() = 0 THEN PLAY MODFILE "A:/tests/TEST.MOD"

  K$ = INKEY$
  IF K$ = "1" THEN PLAY SFX "A:/tests/TEST.WAV"        ' jump
  IF K$ = "2" THEN PLAY SFX "A:/tests/TEST.WAV", 60    ' collect
  IF K$ = "3" THEN PLAY SFX "A:/tests/TEST.WAV", 90    ' collision
  IF K$ = CHR$(27) THEN EXIT DO                        ' ESC

  ' Game work goes here (input, update, draw) instead of blocking on audio.
  LOCATE 1, 1
  PRINT "MUSIC "; PLAYING(); "  EFFECTS "; MM.INFO("SFXVOICES"); "   ";
  PAUSE 10
LOOP

PLAY STOP
