'
'    Implementation of game logic
'
const spr.ball = 1
const spr.paddle = 2
const spr.paddleLong = 3

type GameState
    padX as integer
    padY as integer
    bX as integer
    bY as integer
    bDirX as integer
    bDirY as integer
    quit as integer
    bricks(25, 30) as integer
end type

dim shared state as GameState

'
'    Create the state
'

Sub InitialiseGameState
    state.padX = mm.hres / 2
    state.padY = 470
    state.bX = mm.hres / 2
    state.bY = mm.vres / 2
    state.bDirX = 3
    state.bDirY = -3
    state.quit = 0
    
    local rowIdx as integer, colIdx as integer
    
    for rowIdx = 0 to 20
        for colIdx = 0 to 30
            state.bricks(rowIdx, colIdx) = rowIdx mod 10
        next colIdx
    next rowIdx
end Sub

'
'    Figure out what keys were pressed and adjust state
'
function InterpretKeys(state as GameState) as GameState
    local keyIdx
    
    for keyIdx = 1 to keydown(0)
        if keydown(keyIdx) = 130 then dec state.padX, 3
        if keydown(keyIdx) = 131 then inc state.padX, 3
        if keydown(keyIdx) = 27 then state.quit = 1
    next keyIdx
            
    InterpretKeys = state
end function


'
'    Load graphics into correct buffers
'
Sub LoadGraphics()

    ' load background into page 3
    page write 3    
    load png "gfx/playfield_bg_wide.png"

    ' load sprites
    sprite loadpng spr.ball, "gfx/ball.png"
    sprite loadpng spr.paddle, "gfx/paddle.png"
    sprite loadpng spr.paddleLong, "gfx/paddle_long.png"

    ' load bricks for blitting into page 2
    local brickNames$(10) = ("blue", "red", "green", "cyan", "gold", "yellow", "orange", "pink", "red", "silver", "white")
    local brickIdx as integer

    page write 2    
    for brickIdx = 0 to 10
        load png "gfx/brick_" + brickNames$(brickIdx) + ".png", brickIdx * 32, 0
    next brickIdx

    ' prepare main screen    
    page copy 3 to 0
    page write 0
                
End Sub


'
'    Draw the set of bricks as currently represented by the state
'
Sub DrawBricks(state as GameState)
    ' load bg onto bricks 
    page write 4
    page copy 3 to 4
    
    local row as integer, col as integer, brick as integer
    local offsetX as integer
    
    offsetX = (mm.hres - (25 * 32)) / 2

    ' iterate over field and blit    
    for row = 0 to 25
        for col = 0 to 24
            brick = state.bricks(row, col)
            blit brick * 32, 0, offsetX + col * 32, row * 16, 32, 16, 2
        next col
    next row

    page copy 4 to 0    
    page write 0
End Sub

'
'    This is main loop for this game 
'
Sub RunGame()
    cls
    page write 0
    page display 0

    LoadGraphics()

    InitialiseGameState()
    DrawBricks()
    do
        state = InterpretKeys(state)
    
        ' add direction to current location
        state.bX = state.bX + state.bDirX
        state.bY = state.bY + state.bDirY

        ' did the ball hit a corner? flip the direction        
        if state.bX <= 0 or state.bX > mm.hres - 8 then state.bDirX = state.bDirX * -1
        if state.bY <= 0 or state.bY > mm.vres - 8 then state.bDirY = state.bDirY * -1

        ' how to deal with blocks
        ' - game state should track current column
        ' - it should determine if column has changed and something there, do a bounce
        ' - if something there update that brick, redraw bricks
        ' - the bounce should be based on the column change (xnew <> xold? then bDirX * -1)
        
        ' wait for vertical sync                        
        vsync_wait

        ' move the sprites
        sprite show spr.ball, state.bX, state.bY
        sprite show spr.paddle, state.padX, state.padY
    loop until state.quit = 1
End Sub

