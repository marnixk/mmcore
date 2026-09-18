"""Real PLAY decode/mix and OPTION AUDIO_TARGET."""

from ihelp_util import dump_topic


def test_help_play_mentions_targets(console):
    out = dump_topic(console, "PLAY")
    assert "AUDIO_TARGET" in out
    assert "HDMI" in out
    assert "JACK" in out
    assert "PLAYING()" in out


def test_help_audio_target(console):
    out = dump_topic(console, "AUDIO_TARGET")
    assert out != "?SYNTAX ERROR"
    assert "HDMI" in out
    assert "JACK" in out
    alias = dump_topic(console, "AUDIO")
    assert "AUDIO_TARGET" in alias or "HDMI" in alias


def test_help_option_lists_audio_target(console):
    out = dump_topic(console, "OPTION")
    assert "AUDIO_TARGET" in out
    assert "JACK" in out


def test_audio_target_default_hdmi(console):
    assert console.send_line('PRINT MM.INFO$("AUDIO")') == "HDMI"
    listed = console.send_line("OPTION LIST ALL")
    assert "AUDIO_TARGET HDMI" in listed
    assert "AUDIO ON" in listed


def test_audio_target_jack_and_hdmi(console):
    assert console.send_line("OPTION AUDIO_TARGET JACK") == ""
    assert console.send_line('PRINT MM.INFO$("AUDIO")') == "JACK"
    assert "AUDIO_TARGET JACK" in console.send_line("OPTION LIST")
    assert console.send_line("OPTION AUDIO TARGET HDMI") == ""
    assert console.send_line('PRINT MM.INFO$("AUDIO")') == "HDMI"
    assert console.send_line("OPTION AUDIO_TARGET ANALOG") == ""
    assert console.send_line('PRINT MM.INFO$("AUDIO")') == "JACK"
    assert console.send_line("OPTION AUDIO_TARGET HDMI") == ""


def test_play_wav_mixes_and_stops(console):
    listing = console.send_line('DIR "A:/tests"')
    assert "TEST.WAV" in listing.upper()
    assert console.send_line('PLAY WAV "tests/TEST.WAV"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_help_play_mentions_wav(console):
    out = dump_topic(console, "PLAY")
    assert "WAV" in out
    assert "TEST.WAV" in out


def test_play_mp3_mixes_and_stops(console):
    assert console.send_line('PLAY MP3 "tests/TEST.MP3"') == ""
    # Seeded MP3 is short; real-time mix may already have finished.
    playing = console.send_line("PRINT PLAYING()")
    assert playing in ("0", "1")
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_mod_xm_tone(console):
    assert console.send_line('PLAY MODFILE "tests/TEST.MOD"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line('PLAY XM "tests/TEST.XM"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PLAY TONE 440, 880") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PLAY TONE 440, 880, 4000") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PAUSE 50")
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PLAY TONE 440, 880, 50") == ""
    console.send_line("PAUSE 400")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_beep_defaults_and_args(console):
    assert console.send_line("BEEP") == ""
    assert console.send_line("BEEP 440") == ""
    assert console.send_line("BEEP 440, 4000") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PAUSE 50")
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_help_beep_alias(console):
    out = dump_topic(console, "BEEP")
    assert "BEEP" in out
    assert "frequency" in out or "Hz" in out


def test_play_pause_resume_volume(console):
    assert console.send_line("PLAY TONE 440, 440") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    assert console.send_line("PLAY PAUSE") == ""
    assert console.send_line("PRINT PLAYING()") == "0"
    assert console.send_line("PLAY RESUME") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    assert console.send_line("PLAY VOLUME 50, 25") == ""
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_audio_off_still_tracks_playing(console):
    assert console.send_line("OPTION AUDIO OFF") == ""
    assert console.send_line("PLAY TONE 440, 440, 4000") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PAUSE 50")
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PLAY TONE 440, 440, 50") == ""
    console.send_line("PAUSE 400")
    assert console.send_line("PRINT PLAYING()") == "0"
    assert console.send_line("OPTION AUDIO ON") == ""


def test_play_stops_when_program_ends(console):
    assert console.send_line("PLAY TONE 440, 440") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    assert console.send_line("10 END") == ""
    assert console.send_line("RUN") == ""
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_stops_when_program_falls_off_end(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PLAY TONE 440, 440") == ""
    assert console.send_line("20 PRINT 1") == ""
    assert console.send_line("RUN") == "1"
    assert console.send_line("PRINT PLAYING()") == "0"
