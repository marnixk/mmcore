#include "mmb_priv.h"

static void credits_line(const char *s)
{
	mmb_out(s);
	mmb_out("\n");
}

void mmb_cmd_credits(void)
{
	credits_line("  __  __ __  __ ____            _");
	credits_line(" |  \\/  |  \\/  | __ )  __ _ ___(_) ___");
	credits_line(" | |\\/| | |\\/| |  _ \\ / _` / __| |/ __|");
	credits_line(" | |  | | |  | | |_) | (_| \\__ \\ | (__");
	credits_line(" |_|  |_|_|  |_|____/ \\__,_|___/_|\\___|");
	credits_line("");
	credits_line("    .-------------------------------.");
	credits_line("   /   Colour Maximite homage        /");
	credits_line("  '---------------------------------'");
	credits_line("     [ HDMI ]   bare-metal Pi");
	credits_line("");
	credits_line("MMBasic interpreter");
	credits_line("  Copyright holders: Geoff Graham, Peter Mather");
	credits_line("  Copyright (c) 2021, copyright holders.");
	credits_line("  All rights reserved.");
	credits_line("");
	credits_line("Redistribution and use in source and binary forms,");
	credits_line("with or without modification, are permitted provided");
	credits_line("that the following conditions are met:");
	credits_line("1. Redistributions of source code must retain the");
	credits_line("   above copyright notice, this list of conditions");
	credits_line("   and the following disclaimer.");
	credits_line("2. Redistributions in binary form must reproduce the");
	credits_line("   above copyright notice, this list of conditions");
	credits_line("   and the following disclaimer in the documentation");
	credits_line("   and/or other materials provided with the");
	credits_line("   distribution.");
	credits_line("3. The name MMBasic be used when referring to the");
	credits_line("   interpreter in any documentation and promotional");
	credits_line("   material and the original copyright message be");
	credits_line("   displayed on the console at startup (additional");
	credits_line("   copyright messages may be added).");
	credits_line("4. All advertising materials mentioning features or");
	credits_line("   use of this software must display the following");
	credits_line("   acknowledgement: This product includes software");
	credits_line("   developed by the copyright holder.");
	credits_line("5. Neither the name of the copyright holder nor the");
	credits_line("   names of its contributors may be used to endorse");
	credits_line("   or promote products derived from this software");
	credits_line("   without specific prior written permission.");
	credits_line("");
	credits_line("THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS");
	credits_line("AS IS AND ANY EXPRESS OR IMPLIED WARRANTIES,");
	credits_line("INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES");
	credits_line("OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR");
	credits_line("PURPOSE ARE DISCLAIMED.");
	credits_line("");
	credits_line("PicoMite (RP2040 / RP2350 MMBasic)");
	credits_line("  Geoff Graham, Peter Mather");
	credits_line("");
	credits_line("Circle bare-metal Raspberry Pi environment");
	credits_line("  Copyright (C) R. Stange and contributors");
	credits_line("");
	credits_line("Ported and extended for Raspberry PI by Marnix Kok");
}
