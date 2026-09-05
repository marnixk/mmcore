' will contain information about sprites
const True = 1, False = 0

const MaxSprites = 63

' Index names for array
const SpriteEnabled = 0, SpriteColumns = 1, SpriteRows = 2
const SpriteWidth = 3, SpriteHeight = 4, SpriteFrame = 5, SpriteMaxFrame = 6
const SpriteSourcePage = 7, SpritePageX = 8, SpritePageY = 9
const SpriteX = 10, SpriteY = 11, SpriteVisible = 12, SpriteLayer = 13
const SpriteAnimateEvery = 14, SpriteTick = 15


' Keep track of sprite state 
' 63 sprites, 6 elements
'
'
' - 0 -> 1? then enabled
' - 1 -> number of columns
' - 2 -> number of rows
' - 3 -> width of sprite
' - 4 -> height of sprite
' - 5 -> current sprite frame
' - 6 -> Maximum number of frames (calculated)
' - 7 -> Page to load sprite data from
' - 8 -> X-offset on page to start reading from
' - 9 -> Y-offset on page to start reading from
'
dim integer spriteState(MaxSprites, 15)

'
' Loading a sprite into memory
'
sub sprite.configure (spriteNr%, file$, pageNr%, offsetX%, offsetY%, cols%, rows%, width%, height%)


  if (file$ <> "") then
    page write pageNr%
    load png file$
  end if

  spriteState(spriteNr%, SpriteEnabled) = True
  spriteState(spriteNr%, SpriteColumns) = cols%
  spriteState(spriteNr%, SpriteRows) = rows%
  spriteState(spriteNr%, SpriteWidth) = width%
  spriteState(spriteNr%, SpriteHeight) = height%
  spriteState(spriteNr%, SpriteFrame) = 0
  spriteState(spriteNr%, SpriteMaxFrame) = cols% * rows%
  spriteState(spriteNr%, SpriteSourcePage) = pageNr%
  spriteState(spriteNr%, SpritePageX) = offsetX%
  spriteState(spriteNr%, SpritePageY) = offsetY%
  spriteState(spriteNr%, SpriteX) = 0
  spriteState(spriteNr%, SpriteY) = 0
  spriteState(spriteNr%, SpriteVisible) = False
  spriteState(spriteNr%, SpriteAnimateEvery) = 1
  spriteState(spriteNr%, SpriteTick) = 0

  sprite read spriteNr%, offsetX%, offsetY%, width%, height%, pageNr%

end sub


'
' Copy animation behaviours from another sprite
'
sub sprite.like (spriteNr%, copyFrom%)
  
  ' copy settings across
  spriteState(spriteNr%, SpriteEnabled) = spriteState(copyFrom%, SpriteEnabled)
  spriteState(spriteNr%, SpriteColumns) = spriteState(copyFrom%, SpriteColumns)
  spriteState(spriteNr%, SpriteRows) = spriteState(copyFrom%, SpriteRows)
  spriteState(spriteNr%, SpriteWidth) = spriteState(copyFrom%, SpriteWidth)
  spriteState(spriteNr%, SpriteHeight) = spriteState(copyFrom%, SpriteHeight)
  spriteState(spriteNr%, SpriteMaxFrame) = spriteState(copyFrom%, SpriteMaxFrame)
  spriteState(spriteNr%, SpriteSourcePage) = spriteState(copyFrom%, SpriteSourcePage)
  spriteState(spriteNr%, SpritePageX) = spriteState(copyFrom%, SpritePageX)
  spriteState(spriteNr%, SpritePageY) = spriteState(copyFrom%, SpritePageY)
  spriteState(spriteNr%, SpriteX) = spriteState(copyFrom%, SpriteX)
  spriteState(spriteNr%, SpriteY) = spriteState(copyFrom%, SpriteY)
  spriteState(spriteNr%, SpriteAnimateEvery) = spriteState(copyFrom%, SpriteAnimateEvery)

  spriteState(spriteNr%, SpriteFrame) = 0
  spriteState(spriteNr%, SpriteVisible) = False
  spriteState(spriteNr%, SpriteTick) = 0

  sprite read spriteNr%, spriteState(spriteNr%, SpritePageX), spriteState(spriteNr%, SpritePageY), spriteState(spriteNr%, SpriteWidth), spriteState(spriteNr%, SpriteHeight), spriteState(spriteNr%, SpriteSourcePage)
  
end sub

sub sprite.show(spriteIdx%, x%, y%, layer%)
  spriteState(spriteIdx%, SpriteVisible) = True

  spriteState(spriteIdx%, SpriteX) = x%
  spriteState(spriteIdx%, SpriteY) = y%
  spriteState(spriteIdx%, SpriteLayer) = layer%    

  sprite show spriteIdx%, x%, y%, spriteState(spriteIdx%, SpriteLayer)
end sub

sub sprite.hide(spriteIdx%)
  spriteState(spriteIdx%, SpriteVisible) = False
end sub

sub sprite.speed(spriteIdx%, animateEvery%)
  spriteState(spriteIdx%, SpriteAnimateEvery) = animateEvery%
  spriteState(spriteIdx%, SpriteTick) = 0
end sub


'
' Update all sprite states
'
sub sprite.update
  local integer spriteIdx, sX, sY, row, col, w, h

  for spriteIdx = 0 to MaxSprites

    ' active sprite?
    if spriteState(spriteIdx, SpriteEnabled) = True then

      spriteState(spriteIdx, SpriteTick) = spriteState(spriteIdx, SpriteTick) + 1
      if spriteState(spriteIdx, SpriteTick) < spriteState(spriteIdx, SpriteAnimateEvery) then
        next spriteIdx
      end if

      spriteState(spriteIdx, SpriteTick) = 0
      spriteState(spriteIdx, SpriteFrame) = spriteState(spriteIdx, SpriteFrame) + 1

      ' check frame boundary
      if spriteState(spriteIdx, SpriteFrame) > spriteState(spriteIdx, SpriteMaxFrame) then 
        spriteState(spriteIdx, SpriteFrame) = 0
      end if
  
      ' what next to read?
      row = spriteState(spriteIdx, SpriteFrame) \ spriteState(spriteIdx, SpriteColumns)
      col = spriteState(spriteIdx, SpriteFrame) mod spriteState(spriteIdx, SpriteColumns)

      w = spriteState(spriteIdx, SpriteWidth)
      h = spriteState(spriteIdx, SpriteHeight)
    
      sX = col * w
      sY = row * h

      sprite read spriteIdx, spriteState(spriteIdx, SpritePageX) + sX, spriteState(spriteIdx, SpritePageY) + sY, w, h, spriteState(spriteIdx, SpriteSourcePage)

    end if

  next spriteIdx

  sprite move

end sub

'  _   _                 _                         
' | | | | _____      __ | |_ ___    _   _ ___  ___ 
' | |_| |/ _ \ \ /\ / / | __/ _ \  | | | / __|/ _ \
' |  _  | (_) \ V  V /  | || (_) | | |_| \__ \  __/
' |_| |_|\___/ \_/\_/    \__\___/   \__,_|___/\___|
'                                                  


' load a tileset sprite into screen memory:
' * spriteNr% - The sprite state number to load into (maps state onto actual sprite)
' * file$ - (optional) The file to load
' * pageNr% - (optional) the video page to load file into
' * offsetX% - the x offset on the video page at which the animation lives
' * offsetY% - the y offset on the video page at which the animation lives
' * cols% - the number of columns in the animation
' * rows% - the number of rows in the animation
' * width% - the width of the sprite
' * height% - the height of the sprite
'
' Example: sprite.configure 1, "gfx/explosion.png", 2, 0, 2, 5, 4, 40, 26
'

' To copy animation state into another sprite, use `sprite.like spritenr, fromSpriteNr`
' Example: sprite.like 2, 1

' To adjust the sprite not animation at every tick set the sprite speed
' Example: sprite.speed 2, 5 (animates only every 5 frames)

' Make sure to set tick interrupt to handle sprite animation updates
' Example: settick 100, sprite.update, 1

' use sprite show to show the sprite
' sprite show spriteIdx%, x%, y%, layer%

