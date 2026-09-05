option explicit
option default integer

#include "libs/words_long.inc"
#include "libs/fonts.inc"

' 400x300 8 bit colour
mode 13, 16

' initialise fonts
fnt.init()

dim word_speed = 1

' -- global state 
dim update_tick = 0
dim bg_color = rgb(20, 20, 20)

dim level_up_score = 250
dim energy = 100  
dim score = 0
dim game_over = 0

dim last_key$ = ""

' word vars
dim word$ = words$(0)
dim word_max_nr = 200
dim word_nr = 0
dim word_x = 400
dim word_y = 100
dim float word_wobble = 0

dim laser_x = -20
dim laser_start_x = 40

dim explosion_frame = -1
dim explosion_x = 0
dim explosion_y = 0


'
' Draw the HUD
'
Sub Draw_HUD()

  page write 1

  fnt.draw("score " + str$(score), 10, 265, 0)
  
  local highlight
  local x = 290
  local y = 265
  local bar_bg = rgb(30, 30, 30)
  local border_light = rgb(40, 40, 100)
  local border_dark = rgb(20, 20, 50)
  local e_color = rgb(0, 100, 180)

  fnt.draw("energy", x - 34, y + 1, 0)
  
  box x, y, 100, 10, , bar_bg, bar_bg

  box x, y, energy, 10, , e_color, e_color

  for highlight = 0 to 16
    if energy - highlight >= 0 then
      box x, y, energy - highlight, 10, 0, ,rgb(0, 0, 255 - highlight * 8)
    end if
  next highlight


  ' top left -> top right
  line x, y, x + 100, y,, border_light

  ' top left -> bottom left
  line x, y, x, y + 10,, border_light

  ' top right -> top bottom
  line x + 100, y, x + 100, y + 10,, border_dark

  ' bottom left -> bottom right
  line x, y + 10, x + 100, y + 10,, border_dark

  

End Sub

'
' Should draw the Laser
'
Sub Draw_Laser(x, steps, thickness)
  page write 1

  local col, colr
  local per_step = 255 / steps
  local current_colour = 0

  local near_x, far_x, color


  ' draw the laser outside
  for col = 0 to steps
    near_x = x - steps + col
    far_x = x + steps + thickness - col
    colr = rgb(min(255, current_colour), min(255, current_colour), max(0, 128 - current_colour / 2))

    line near_x, 0, near_x, 300, 1, colr
    line far_x, 0, far_x, 300, 1, colr

    current_colour = current_colour + per_step
  next col

  
  box x, -1, thickness, 302, , rgb(255, 255, 255), rgb(255, 255, 255)
End Sub

'
' Draw the word
'
Sub Draw_Word()
  page write 1

  local char_nr
1  local char_x, char_y

  for char_nr = 1 to len(word$)
    char_x = word_x + char_nr * 9
    char_y = int(word_y + sin(word_wobble + (char_nr * 0.3)) * 5)
    fnt.draw(mid$(word$, char_nr, 1), char_x, char_y, 4)
  next char_nr
  

End Sub

'
' Draw explosion
'
Sub Draw_Explosion

  if explosion_frame = -1 then return

  local circle_nr
  local r = explosion_frame * -2
  local cx, cy
  local ccol

  page write 4
  cls

  for circle_nr = 0 to 10

    cx = explosion_x - (r/2) + (cos(rnd()) * r)
    cy = explosion_y - (r/2) + (sin(rnd()) * r)

    circle cx, cy, rnd() + 2, , , rgb(white), rgb(white)

  next circle_nr
  
  page xor_pixels 4, 1, 1

  
End Sub

'
' Reset the global variables for a new game
'
Sub Game.Reset()
  energy = 100
  score = 0
  game_over = 0
  last_key$ = ""

  explosion_frame = 1

  ' new word
  word$ = words$(int(rnd() * word_max_nr))
  word_x = 400

  ' reset laser
  laser_x = -20
  laser_start_x = 40
End Sub



'
' Called 30 times per secod
'
Sub Game.Update()


  update_tick = update_tick + 1

  ' laser not in its proper place yet?
  if laser_x < laser_start_x then laser_x = laser_x + 1
  
  ' the word should go left
  if update_tick mod word_speed = 0 then word_x = word_x - 1 

  ' someone set explosion_frame to be not -1? let's animate it.
  if explosion_frame >= 0 then explosion_frame = explosion_frame + 1
  if explosion_frame > 20 then explosion_frame = -1

  ' wobble word
  word_wobble = word_wobble + 0.3

  ' check key was pressed
  local key$ = inkey$
  local key_nr = asc(key$)
  if key_nr > 0 then Game.CheckKey(key$)
  
  Game.CheckCollision()

End Sub

'
' check letter touching laser
'
Sub Game.CheckCollision()
  if word_x <= laser_x then
    explosion_x = word_x
    explosion_y = word_y
    explosion_frame = 0

    energy = energy - 5

    ' remove letter
    word$ = mid$(word$, 2, len(word$))
    word_x = word_x + 5

    play stop
    play wav "sfx/nope.wav"

    if len(word$) = 0 then
      Game.NextWord()
    end if
  end if

  if energy <= 0 then game_over = 1

End Sub

Sub scoreKaching
  play stop
  play wav "sfx/score.wav"
end sub


'
' Determine whether the next letter has been pressed
'
Sub Game.CheckKey(key$)
  local word_first_char$ = mid$(word$, 1, 1)

  ' same letter? cut it off.
  if key$ = word_first_char$ then
    word$ = mid$(word$, 2, len(word$))
    word_x = word_x + 9
    score = score + 15
    laser_start_x = 40 + int(score / level_up_score) * 20
  end if

  if asc(key$) = 27 then game_over = 1

  if asc(key$) >= asc("a") and asc(key$) <= asc("z") then
    play stop

    if word$ = "" then
      play tts key$,,,,, scoreKaching
    else
      play tts key$
    end if
  end if

  if word$ = "" then 
    Game.NextWord()
  end if

End Sub


'
' Set the next word
'
Sub Game.NextWord()
  local current_word = word_nr

  word_x = 400
  word_y = int(rnd() * 200) + 30

  ' set a new word until we are using a different than current word
  do
    word_nr = int(rnd() * word_max_nr)
  loop until word_nr <> current_word

  word$ = words$(word_nr)  

End Sub



'
' Draw the game screen
'
Sub Game.Screen()

  ' prepare page 2 with a rounded box for masking
  page write 2
  box 0, 0, 400, 400, rgb(black), rgb(black)
  rbox 10, 10, 380, 250, 20, rgb(white), rgb(white)

  settick 35, Game.Update

  do
    page write 1

    ' reset background
    box 0, 0, 400, 300, , bg_color, bg_color

    ' Draw Word
    Draw_Word()
    
    ' Draw laser
    Draw_Laser(laser_x, 2 + int(rnd() * 8), int(rnd()*2) + 3)

    ' If explosion triggered, will draw.
    Draw_Explosion()
  
    ' mask round box on laser and copy to screen
    page and_pixels 2, 1, 1
    
    Draw_HUD()

    page copy 1, 0

  loop until game_over = 1

  page write 0

End Sub


Sub Instructions.Screen()
  page write 0
  cls

  local y = 50

  fnt.draw "instructions", 10, y + 0, 4
  fnt.draw "syntax shock is a little typing game inspired", 10, y + 30, 3
  fnt.draw "by radarsoft's tempo typen. words float from", 10, y + 40, 3
  fnt.draw "the right side of the screen, to the left.", 10, y + 50, 3
  fnt.draw "a laser is waiting to gobble up the letters,", 10, y + 70, 3
  fnt.draw "so make sure to type the letters on time!", 10, y + 80, 3
  fnt.draw "when a letter is destroyed by the laser, you", 10, y + 100, 3
  fnt.draw "lose energy.", 10, y + 110, 3
  fnt.draw "lose all your energy and the game is over!", 10, y + 130, 3
  fnt.draw "start in kids mode for a slower experience", 10, y + 150, 2
  
  fnt.draw "press enter to continue", 10, 280, 0

  do: loop until asc(inkey$) = 13
End Sub


'
' Draw the menu and deal with interactions
'
Sub Menu.Screen()
  local pressed$
  local last_pressed$
  local active_item = 0
  local label$
  local menu_idx = 0
  local items$(3) = ("start kids mode", "start", "instructions", "quit")

draw_menu:
  page write 0
  load jpg "gfx/syntax-shock-small.jpg"


  do

    for menu_idx = 0 to 3
      label$ = items$(menu_idx)
      
      if active_item = menu_idx then
        fnt.draw label$, 200 - len(label$) * 4, 230 + (menu_idx * 10), 3
      else
        fnt.draw label$, 200 - len(label$) * 4, 230 + (menu_idx * 10), 2
      end if
    next menu_idx

    pressed$ = inkey$

    ' up pressed?   
    if asc(pressed$) = 128 and active_item > 0 then active_item = active_item - 1

    ' down pressed?
    if asc(pressed$) = 129 and active_item < 3 then active_item = active_item + 1

    ' enter pressed?
    if asc(pressed$) = 13 then
      
      if active_item = 0 then
        word_speed = 5
        Game.Reset()
        Game.Screen()
        goto draw_menu
      elseif active_item = 1 then
        word_speed = 1
        Game.Reset()
        Game.Screen()
        goto draw_menu
      elseif active_item = 2 then
        Instructions.Screen()
        goto draw_menu
      else if active_item = 3 then
        exit
      end if

    end if        

  ' loop until escape pressed
  loop until asc(pressed$) = 27  

End sub

'Instructions.Screen()

Menu.Screen()

' start the game
' Game.Screen()


