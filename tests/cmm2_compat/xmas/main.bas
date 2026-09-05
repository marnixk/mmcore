option default integer
option explicit on


const True = 1
const False = 0


'
' INITIALISATION
'
'
mode 7, 12

dim integer g_started = 0
dim integer g_gameOver = False

#include "includes/keyboard.inc"
#include "includes/fonts.inc"
#include "includes/fades.inc"
#include "screen_menu.inc"

fnt.init()
keyb.Initialise()

'
' Constants to keep track of the sprite numbers
'
const spr.Gift1 = 1
const spr.Gift2 = 2
const spr.Gift3 = 3
const spr.Paddle = 4
const spr.GrinchLg = 5
const spr.GrinchHideLg = 6
const spr.GrinchSmLeft = 7
const spr.GrinchHideSmLeft = 8
const spr.GrinchSmRight = 9
const spr.GrinchHideSmRight = 10

const spr.GrinchXs = 11
const spr.GrinchHideXs = 12
const spr.Glove = 13
const spr.GloveWhack = 14

dim integer glove.pos.x(3) = (130, 210, 150, 170)
dim integer glove.pos.y(3) = (120, 110, 80, 160)

dim integer state.inGame = False
dim integer state.frame = 0
dim integer state.padX = 60
dim integer state.whack = False
dim integer state.whacked = -1
dim integer state.lostGift = -1
dim integer state.foundGift = -1
dim integer state.glovePos = 0

' ------------------------------
'  Falling Gifts State
' ------------------------------

const N_Gifts = 25
const Gifts_TopSize = 2000
const IncreaseDiffEvery = 5

dim integer state.activeGifts = 5
dim integer state.gift.x(N_Gifts)
dim integer state.gift.y(N_Gifts)
dim integer state.lostAmount = 0


' ------------------------------
'   Keep score
' ------------------------------

dim integer score.highest = 0
dim integer score.gifts = 0
dim integer score.whacked = 0


' ------------------------------
'  Grinch sprite state
' ------------------------------


' current tick since last update
dim integer state.grinchUpdateStep = 0

' amount of ticks to wait for an animation update
dim integer state.grinchUpdateFreq = 100

' current animation frame for each grinch, < 0 means invisible
dim integer state.grinchFrame(3) = (-20, -4, -8, -12)

dim integer state.grinchTick = False

' for grinch number + (animation_frame * 4) = sprite number to show
dim grinchPos.sprite(7) = (spr.GrinchHideSmLeft, spr.GrinchHideSmRight, spr.GrinchHideXs, spr.GrinchHideLg, spr.GrinchSmLeft, spr.GrinchSmRight, spr.GrinchXs, spr.GrinchLg)

                             ' hidden grinch coords   eating grinch coords    
dim integer grinchPos.x(7) = (180, 265, 210, 208,     180, 265, 210, 213)
dim integer grinchPos.y(7) = (147, 139, 118, 177,     120, 113, 102, 123)




'
' Initialise the game by loading PNGs into Sprites
'
sub game.init()

  '' -- page 5 is for the playfield background
  page write 5
  cls

  load png "gfx/breakout-bg.png"
  load png "gfx/whack-bg.png", 160, 0

  ' middle bar
  line 159, 0, 159, 240, 5, rgb(15, 15, 50)
  line 160, 0, 160, 240, 3, rgb(25, 25, 150)


  page write 4


  load png "gfx/gift_1_sm.png", 0, 0, 4
  load png "gfx/gift_2_sm.png", 0, 36, 4
  load png "gfx/gift_3_sm.png", 0, 72, 4

  sprite loadpng spr.Paddle, "gfx/paddle.png", 4

  page write 2

  sprite loadpng spr.GrinchLg, "gfx/eat-grinch.png", 4
  sprite loadpng spr.GrinchHideLg, "gfx/hidden-grinch.png", 4
  sprite loadpng spr.GrinchSmLeft, "gfx/eat-grinch-sm.png", 4
  sprite loadpng spr.GrinchHideSmLeft, "gfx/hidden-grinch-sm.png", 4
  sprite loadpng spr.GrinchSmRight, "gfx/eat-grinch-sm.png", 4
  sprite loadpng spr.GrinchHideSmRight, "gfx/hidden-grinch-sm.png", 4
  sprite loadpng spr.GrinchXs, "gfx/eat-grinch-xs.png", 4
  sprite loadpng spr.GrinchHideXs, "gfx/hidden-grinch-xs.png", 4

  ' load glove
  sprite loadpng spr.Glove, "gfx/santa-whack-sm", 4

  ' draw and rotate glove, then turn rotated glove into sprite
  sprite show spr.Glove, 0, 0, 2
  image rotate 0, 0, 70, 70, 0, 70, 90, 2
  sprite read spr.GloveWhack, 5, 70, 70, 45, 2

  ' hide sprite
  sprite hide spr.Glove

  
  game.resetState()

end sub



sub game.resetState()

  g_gameOver = False

  state.activeGifts = 5
  score.highest = 0
  score.gifts = 0
  score.whacked = 0
  state.grinchUpdateFreq = 100
  state.lostAmount = 0
  state.foundGift = -1
  state.lostGift = -1
  state.whacked = -1

  ' ------------------------------
  '  Initialie gifts
  ' ------------------------------

  local integer giftIdx
  
  for giftIdx = 0 to N_Gifts 
    state.gift.x(giftIdx) = int(rnd() * 120) + 5

    ' distribute over top of the screen
    state.gift.y(giftIdx) = int(rnd() * -2000) 
  next giftIdx

end sub


'
' The game state is updated when his function is called by the timer interrupt
'
sub game.updateState()
  
  if state.inGame = False then return

  state.frame = state.frame + 1

  if keyboard(K_Z) then state.padX = state.padX - 3
  if keyboard(K_V) then state.padX = state.padX + 3

  '
  ' if not holding an arrow key, it's in the middle
  '
  if keyboard(K_Left) then state.glovePos = 0
  if keyboard(K_Right) then state.glovePos = 1
  if keyboard(K_Up) then state.glovePos = 2
  if keyboard(K_Down) then state.glovePos = 3

  if keyboard(K_Middle) then 
    state.whack = True
  else 
    state.whack = False
  end if


  '
  ' keep paddle within bounds
  '
  if state.padX < -2 then state.padX = -2
  if state.padX > 126 then state.padX = 126


  '
  ' if something recently whacked, let's decrease the label timer
  '
  if state.whacked >= 0 then state.whacked = state.whacked - 1

  '
  ' if lost a gift recently, let's decreate label timer
  '
  if state.lostGift >= 0 then state.lostGift = state.lostGift - 1  

  '
  ' if found a gift recently, let's decreate label timer
  '
  if state.foundGift >= 0 then state.foundGift = state.foundGift - 1  


  '
  ' if the glove is in whack state and there is a grinch there, hide it.
  ' 
  if state.whack = True and state.grinchFrame(state.glovePos) > -1 then
    state.grinchFrame(state.glovePos) = int(rnd() * -20)
    state.whacked = 25
    score.whacked = score.whacked + 1    
    play effect "sound/bonk.wav"
  end if




  '
  ' let's figure out if we need to update te grinch animation step
  '
  state.grinchUpdateStep = state.grinchUpdateStep + 1
  if state.grinchUpdateStep >= state.grinchUpdateFreq then
    state.grinchUpdateStep = 0
    game.updateGrinch()
  end if


  '
  ' Update the falling gifts locations
  '
  local integer giftIdx, x, y
  
  for giftIdx = 0 to state.activeGifts
    
    x = state.gift.x(giftIdx)
    y = state.gift.y(giftIdx)

    state.gift.y(giftIdx) = y + 1

    
    ' gift hit paddle?
    if y = 220 and x + 32 >= state.padX and x <= state.padX + 40 then
      state.gift.y(giftIdx) = int(rnd() * -2000)
      state.gift.x(giftIdx) = int(rnd() * 120) + 5

      score.gifts = score.gifts + 1
      state.foundGift = 50

      if score.gifts > score.highest then
        score.highest = score.gifts
      end if 

      play effect "sound/score.wav"

      ' when they reach 11, 21, 31 make it harder
      if score.gifts mod IncreaseDiffEvery = 1 then
        state.activeGifts = state.activeGifts + 1
        if state.activeGifts > N_Gifts then state.activeGifts = N_Gifts

        state.grinchUpdateFreq = state.grinchUpdateFreq - 10
        if state.grinchUpdateFreq < 20 then state.grinchUpdateFreq = 20
        
      end if
      
    end if

    ' gift out of screen? reset location, and reduce caught gifts
    if state.gift.y(giftIdx) > 240 + 32 then 
      state.gift.y(giftIdx) = int(rnd() * -2000)
      state.gift.x(giftIdx) = int(rnd() * 120) + 5

      if score.gifts > 0 then score.gifts = score.gifts - 1

      if score.gifts < 0 then g_gameOver = True

      state.lostAmount = 1
      state.lostGift = 50

      play effect "sound/oof.wav"

    end if

  next giftIdx


end sub


'
' The grinch frame states will be updated
'
sub game.updateGrinch()
  
  state.grinchTick = true

  '
  ' for each grinch update animation frame number if active
  '
  local integer grinch
  for grinch = 0 to 3 

    state.grinchFrame(grinch) = state.grinchFrame(grinch) + 1

    ' reset when hit max frame      
    if state.grinchFrame(grinch) > 1 then 
      state.lostGift = 50
      state.lostAmount = 5
      score.gifts = score.gifts - 5

      ' if hit, let's give the player more time for that grinch hole
      state.grinchFrame(grinch) = int(rnd() * -50)

      if score.gifts < 0 then g_gameOver = True

      play effect "sound/oof.wav"
    end if
       
  next grinch

end sub


sub screen.gameOver()

  local integer y

  '
  ' Show the gameover screen
  '
  state.inGame = False    
  fades.fadeOut()

  local string scoreText = "you scored " + str$(score.highest * 25 + score.whacked * 5) + "!"
  local string whackText = "grinches whacked " + str$(score.whacked)

  page write 0

  cls
  for y = 0 to 63
    line 0, 63 - y, 320, 63 - y, 1, rgb(y * 4, 0, 0)
  next y

  load png "gfx/santa.png", 110, 10, 4


  ' 40, 120, 5
  fnt.draw "game over santa", 100, 120, 4
  fnt.draw scoreText, 160 - (len(scoreText) * 4), 150, 2
  fnt.draw whackText, 160 - (len(whackText) * 4), 162, 2
  fnt.draw "press enter", 115, 230, 3

  fades.fadeIn()  
  
  do
     keyb.GatherKeyInfo()
  loop until keyboard(K_Enter) = True


end sub


'
' Draw the current state of the game screen
'
sub screen.game()

  page write 2
  cls


  page write 0
  cls

  '
  ' page 5 contains the game background
  '
  page copy 5, 0
  fades.fadeInMiddle()

  
  state.inGame = True

  local integer gIdx, gSprite, gFrame, gX, gY

  page write 2


  local integer giftIdx, giftSrcY, y

  do
    cls

    '
    ' Draw Gifts
    '
    ' - page 4 has the gift images
    '
    for giftIdx = 0 to state.activeGifts
      if state.gift.y(giftIdx) > 0 then
        giftSrcY = (giftIdx mod 3) * 36
        blit 0, giftSrcY, state.gift.x(giftIdx), state.gift.y(giftIdx) - 36, 32, 36, 4
      end if
    next giftIdx

    
    '
    ' Draw paddle
    '
    sprite show spr.Paddle, state.padX, 220, 2


    '
    ' first move all grinch sprites offscreen
    '
    for gIdx = 0 to 7
      sprite show grinchPos.sprite(gIdx), 319, 239, 2
    next gIdx



    '
    ' Draw active grinches
    '
    for gIdx = 0 to 3
      gFrame = state.grinchFrame(gIdx)
      if gFrame > -1 then
        gSprite = grinchPos.sprite(gIdx + (gFrame * 4))
        gX = grinchPos.x(gIdx + (gFrame * 4))
        gY = grinchPos.y(gIdx + (gFrame * 4))  
        sprite show gSprite, gX, gY, 2

      end if

      ' text 0, gIdx * 10, str$(gFrame) + " " + str$(gSprite) + " x,y: " + str$(gX) + ", " + str$(gY) + " -> gf " + str$(gFrame)
    next gIdx


        

    '
    ' show the correct glove sprite
    '
    if state.whack = False then 
      
      sprite show spr.Glove, glove.pos.x(state.glovePos), glove.pos.y(state.glovePos), 2
      sprite show spr.GloveWhack, 319, 239, 1

    else

      sprite show spr.GloveWhack, glove.pos.x(state.glovePos) + 7, glove.pos.y(state.glovePos) - 2, 2
      sprite show spr.Glove, 319, 239, 1

    end if


    if state.foundGift > 0 then
      y = int(sin(state.foundGift * 0.1) * 5)

      ' lost a gift    
      blit 0, 0, 140, 35 - y, 32, 36, 4
      fnt.draw "+1", 168, 48 - y, 3

    end if



    if state.lostGift > 0 then
      y = int(sin(state.lostGift * 0.1) * 5)

      ' lost a gift    
      blit 0, 0, 140, 195 + y, 32, 36, 4
      fnt.draw "-" + str$(state.lostAmount), 168, 208 + y, 3

    end if

    
    '
    ' if recently something was whacked then show a label
    '
    if state.whacked > 0 then    

      fnt.draw "whack!", 202, 218, 5    

    end if

    fnt.draw str$(score.highest * 25 + score.whacked * 5), 5, 5, 3
    fnt.draw "gifts " + str$(score.gifts), 5, 230, 2

'    fnt.draw "whacked  " + str$(score.whacked), 122, 12, 0


    '
    ' page copy to random place to wait for vsync
    '
    page copy 2, 1, b


    '
    ' keyboard handling
    ' 
    keyb.GatherKeyInfo()    
    if keyboard(K_Escape) then goto exit_game

  loop until g_gameOver = True


  screen.gameOver()
 
  exit_game:  

    state.inGame = False
    fades.fadeOut()
    fades.soundFadeOut()

end sub




settick 10, game.updateState

game.init()

do


  screen.menu()  
  screen.game()
 
  game.resetState()

  ' restart music
  'play stop

loop
