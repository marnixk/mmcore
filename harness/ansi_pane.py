"""TERM-like 80-column ANSI pane used to render termlog host bytes."""

from __future__ import annotations

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

PAL = [
    (0, 0, 0),
    (170, 0, 0),
    (0, 170, 0),
    (170, 85, 0),
    (0, 0, 170),
    (170, 0, 170),
    (0, 170, 170),
    (170, 170, 170),
    (85, 85, 85),
    (255, 85, 85),
    (85, 255, 85),
    (255, 255, 85),
    (85, 85, 255),
    (255, 85, 255),
    (85, 255, 255),
    (255, 255, 255),
]


class AnsiPane:
    def __init__(self, cols: int = 80, rows: int = 29) -> None:
        self.cols = cols
        self.rows = rows
        self.cell = [[" "] * cols for _ in range(rows)]
        self.fg = [[7] * cols for _ in range(rows)]
        self.bg = [[0] * cols for _ in range(rows)]
        self.row = 0
        self.col = 0
        self.cur_fg = 7
        self.cur_bg = 0
        self.bold = False
        self.inv = False
        self.sav = (0, 0)
        self.ansi_st = 0
        self.ansi_priv = False
        self.args: list[int] = []
        self.iac = 0
        self.iac_cmd = 0
        self.sb = False
        self.fed = 0

    def snapshot(self) -> list[str]:
        return ["".join(r) for r in self.cell]

    def occupancy(self, c0: int, c1: int) -> int:
        n = 0
        for r in self.cell:
            for c in range(c0, min(c1, self.cols)):
                if r[c] not in (" ", "\0"):
                    n += 1
        return n

    def _pen(self) -> int:
        n = self.cur_fg
        if self.bold and n < 8:
            n += 8
        return n

    def _paper(self) -> int:
        return self.cur_bg

    def _put(self, ch: str) -> None:
        if self.col >= self.cols:
            self._nl()
        if self.col < self.cols and 0 <= self.row < self.rows:
            fg, bg = self._pen(), self._paper()
            if self.inv:
                fg, bg = bg, fg
            self.cell[self.row][self.col] = ch
            self.fg[self.row][self.col] = fg
            self.bg[self.row][self.col] = bg
            self.col += 1

    def _nl(self) -> None:
        if self.row >= self.rows - 1:
            self.cell.pop(0)
            self.fg.pop(0)
            self.bg.pop(0)
            self.cell.append([" "] * self.cols)
            self.fg.append([7] * self.cols)
            self.bg.append([0] * self.cols)
            self.row = self.rows - 1
            self.col = 0
            return
        self.row += 1
        self.col = 0

    def _cup(self, row: int, col: int) -> None:
        self.row = max(0, min(self.rows - 1, row - 1))
        self.col = max(0, min(self.cols - 1, col - 1))

    def _erase_line(self, mode: int) -> None:
        a, b = 0, self.cols
        if mode == 0:
            a = self.col
        elif mode == 1:
            b = self.col + 1
        for c in range(a, b):
            self.cell[self.row][c] = " "
            self.fg[self.row][c] = self._pen()
            self.bg[self.row][c] = self._paper()

    def _erase_disp(self, mode: int) -> None:
        a, b = 0, self.rows
        if mode == 0:
            a = self.row
        elif mode == 1:
            b = self.row + 1
        for r in range(a, b):
            self.cell[r] = [" "] * self.cols
            self.fg[r] = [self._pen()] * self.cols
            self.bg[r] = [self._paper()] * self.cols
        if mode in (2, 3):
            self.row = 0
            self.col = 0

    def _arg(self, i: int, dflt: int) -> int:
        if i < len(self.args) and self.args[i] > 0:
            return self.args[i]
        return dflt

    def _sgr(self) -> None:
        args = self.args or [0]
        i = 0
        while i < len(args):
            v = args[i]
            if v == 0:
                self.cur_fg, self.cur_bg = 7, 0
                self.bold = False
                self.inv = False
            elif v == 1:
                self.bold = True
            elif v == 7:
                self.inv = True
            elif v == 22:
                self.bold = False
            elif v == 27:
                self.inv = False
            elif 30 <= v <= 37:
                self.cur_fg = v - 30
            elif 90 <= v <= 97:
                self.cur_fg = v - 90 + 8
            elif 40 <= v <= 47:
                self.cur_bg = v - 40
            elif 100 <= v <= 107:
                self.cur_bg = v - 100 + 8
            elif v == 39:
                self.cur_fg = 7
            elif v == 49:
                self.cur_bg = 0
            i += 1

    def _csi(self, cmd: str) -> None:
        n = self._arg(0, 1)
        if cmd == "A":
            self.row = max(0, self.row - n)
        elif cmd == "B":
            self.row = min(self.rows - 1, self.row + n)
        elif cmd == "C":
            self.col = min(self.cols - 1, self.col + n)
        elif cmd == "D":
            self.col = max(0, self.col - n)
        elif cmd == "G":
            self._cup(self.row + 1, self._arg(0, 1))
        elif cmd == "d":
            self._cup(self._arg(0, 1), self.col + 1)
        elif cmd == "H" or cmd == "f":
            self._cup(self._arg(0, 1), self._arg(1, 1))
        elif cmd == "J":
            self._erase_disp(self.args[0] if self.args else 0)
        elif cmd == "K":
            self._erase_line(self.args[0] if self.args else 0)
        elif cmd == "m":
            self._sgr()
        elif cmd == "s":
            self.sav = (self.row, self.col)
        elif cmd == "u":
            self._cup(self.sav[0] + 1, self.sav[1] + 1)
        elif cmd == "X":
            for c in range(self.col, min(self.cols, self.col + n)):
                self.cell[self.row][c] = " "

    def _ansi(self, b: int) -> bool:
        if self.ansi_st == 0:
            if b == 27:
                self.ansi_st = 1
                return True
            return False
        if self.ansi_st == 1:
            if b == ord("["):
                self.ansi_st = 2
                self.ansi_priv = False
                self.args = []
                return True
            self.ansi_st = 0
            return True
        if 48 <= b <= 57:
            if not self.args:
                self.args = [0]
            self.args[-1] = self.args[-1] * 10 + (b - 48)
            return True
        if b == ord(";"):
            self.args.append(0)
            return True
        if b in (ord("?"), ord(">"), ord("=")):
            self.ansi_priv = True
            return True
        if (65 <= b <= 90) or (97 <= b <= 122):
            if not self.ansi_priv:
                self._csi(chr(b))
            self.ansi_st = 0
            return True
        self.ansi_st = 0
        return True

    def feed_byte(self, b: int) -> None:
        self.fed += 1
        if self.sb:
            if b == IAC:
                self.iac = 1
            elif self.iac == 1 and b == SE:
                self.sb = False
                self.iac = 0
            else:
                self.iac = 0
            return
        if self.iac == 1:
            if b == IAC:
                self.iac = 0
                self._put(chr(b))
                return
            if b == SB:
                self.iac = 0
                self.sb = True
                return
            if b in (WILL, WONT, DO, DONT):
                self.iac_cmd = b
                self.iac = 2
                return
            self.iac = 0
            return
        if self.iac == 2:
            self.iac = 0
            return
        if b == IAC:
            self.iac = 1
            return
        if self._ansi(b):
            return
        if b == 0:
            return
        if b == 12:
            self._erase_disp(2)
            return
        if b == 13:
            self.col = 0
        elif b == 10:
            self._nl()
        elif b in (8, 127):
            if self.col > 0:
                self.col -= 1
                self.cell[self.row][self.col] = " "
        elif b >= 32:
            self._put(chr(b))

    def feed(self, data: bytes) -> None:
        for b in data:
            self.feed_byte(b)
