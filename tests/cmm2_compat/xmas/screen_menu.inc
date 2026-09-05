sub screen.menu()


  page write 1
  box 0, 0, 320, 240, 1, rgb(0, 0, 0, 15), rgb(0, 0, 0, 15)


  '
  '  SCENE START
  '
  '
  page write 0
  load png "gfx/home.png"

  load png "gfx/vs-image.png", 94, 75, 4

  fnt.draw "santa's catch", 110, 15, 4

  fnt.draw "catch the gifts and", 82, 40, 2
  fnt.draw "whack the grinch!", 90, 50, 2
  
  fnt.draw "press enter!", 115, 230, 3
  
  
  const N_Snow = 512
  
  local float snow_x(N_Snow)
  local float snow_y(N_Snow)
  local float snow_speed(N_Snow)
  
  local integer i
  
  ' 
  ' initialise the snow
  '
  for i = 0 to N_Snow
    snow_x(i) = int(rnd() * 320)
    snow_y(i) = int(rnd() * -240)
    snow_speed(i) = rnd() * 4
  next i


  '
  ' Start music
  '
  if g_started = False then
    play modfile "sound/jingle2.mod"
  else
    fades.soundFadeIn()
  end if

  g_started = True

  fades.fadeInMiddle()
   
  
  page write 3
  
  local float angle = 0, x_plus
  
  do
  
    ' ----------------------------------
    '
    '     draw the scene
    '
    ' ----------------------------------
  
  
    cls  
    color rgb(255, 255, 255, 10)
    pixel snow_x(), snow_y()
  
    page copy 3, 1
  
    ' ----------------------------------
    '
    '     move all the snow around
    '
    ' ----------------------------------

    x_plus = sin(angle) 
  
    for i = 0 to N_Snow
      snow_y(i) = snow_y(i) + snow_speed(i)
      snow_x(i) = (snow_x(i) + (snow_speed(i) * x_plus)) mod 320
  
      if snow_y(i) > 240 then snow_y(i) = 0
      if snow_x(i) < 0 then snow_x(i) = 320
    next i

    ' adjust the angle at which the snow should fall
    angle = angle + 0.02


    keyb.GatherKeyInfo()
  
    if keyboard(K_Enter) then
      goto exit_loop
    end if  

  loop

  exit_loop:  

  fades.fadeOut()
  
end sub





'#include "includes/fonts.inc"
'#include "includes/keyboard.inc"

'mode 7, 12

'fnt.init()
'keyb.Initialise()
'screen.menu()

