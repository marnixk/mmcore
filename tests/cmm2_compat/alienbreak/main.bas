'
'    Author: Marnix Kok - 2026
'
#include "game.bas"

' initialise, hide cursor
mode 10, 16
locate ,,0
option explicit on

const Choice.Start = 0
const Choice.Exit = 1


'
'    Show the intro screen
'
Function IntroScreen()

    page display 0
    page write 2
    
    load png "gfx/intro_screen_wide.png", 0, 0
    page copy 2 to 0
    page write 0

    local menuItems$(2) = ("Start", "Exit")
    local activeIdx% = 0            
    local color%
    local i%
    local key$

    do
        ' iterate over menu items
        for i% = 0 to 2
        
            ' current item active?
            if i% = activeIdx% then 
                color% = rgb(white) 
            else 
                color% = rgb(0, 200, 0)
            end if
            
            ' show menu item            
            text 424 - (len(menuItems$(i%)) * 4), 420 + i% * 18, menuItems$(i%), color%
        next i

        ' read keys
        key$ = inkey$()
        if key$ = chr$(128) then activeIdx% = max(activeIdx% - 1, 0)
        if key$ = chr$(129) then activeIdx% = min(activeIdx% + 1, 1)    
            
        ' wait.
        vsync_wait
    loop until key$ = chr$(13)
    
    IntroScreen = activeIdx%


End Function

dim choice%
do
    choice% = IntroScreen()
    if choice% = Choice.Start then
        RunGame()
    end if
                          
loop until choice% = Choice.Exit 

mode 11
cls

