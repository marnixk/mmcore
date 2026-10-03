"""Real PLAY decode/mix and OPTION AUDIO_TARGET."""

import time

from ihelp_util import dump_topic


def _play_until_stopped(console, command: str, timeout: float = 15.0) -> bool:
    """Start a track and poll PLAYING() until it ends on its own."""
    assert console.send_line(command) == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if console.send_line("PRINT PLAYING()") == "0":
            return True
        time.sleep(0.4)
    return False


def test_help_play_mentions_targets(console):
    out = dump_topic(console, "PLAY")
    assert "AUDIO_TARGET" in out
    assert "HDMI" in out
    assert "JACK" in out
    assert "PLAYING()" in out
    assert "MIXGAP" in out


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


def test_play_mix_gap_stays_short(console):
    """#936: while the poll loop is idle the mixer is serviced often enough
    that the latched gap stays well under the queued cushion."""
    assert console.send_line('PLAY S3M "tests/TEST.S3M"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    time.sleep(0.4)
    gap = int(console.send_line('PRINT MM.INFO("MIXGAP")'))
    underrun = console.send_line('PRINT MM.INFO("UNDERRUN")')
    console.send_line("PLAY STOP")
    assert gap < 200, gap
    assert underrun == "0"


def test_play_s3m(console):
    assert console.send_line('PLAY S3M "tests/TEST.S3M"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"
    assert console.send_line('PLAY S3MFILE "tests/TEST.S3M"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_s3m_missing_file_errors(console):
    assert console.send_line('PLAY S3M "tests/NOPE.S3M"') == "?S3M"


def test_play_tracker_modules_stop_at_song_end(console):
    """#924: MOD/XM/S3M play once and report a natural end.

    The one-pattern fixtures take ~8 s; if an engine still looped forever this
    poll would time out and JUKE would never advance its queue.
    """
    assert _play_until_stopped(console, 'PLAY MODFILE "tests/TEST.MOD"')
    assert _play_until_stopped(console, 'PLAY XM "tests/TEST.XM"')
    assert _play_until_stopped(console, 'PLAY S3M "tests/TEST.S3M"')


def test_help_play_mentions_s3m(console):
    out = dump_topic(console, "PLAY")
    assert "S3M" in out
    assert "TEST.S3M" in out


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


def test_play_reset_reinitialises_backend(console):
    """#1024: PLAY RESET re-creates the audio backend without a reboot.

    The command dispatch and state reset are testable under QEMU even though
    the deviceless harness cannot exercise the Circle DMA path itself.
    """
    before = int(console.send_line('PRINT MM.INFO("AUDIORESET")'))
    assert console.send_line("PLAY TONE 440, 440") == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    assert console.send_line("PLAY RESET") == ""
    assert console.send_line('PRINT MM.INFO("AUDIORESET")') == str(before + 1)
    # The decoder/engine are kept, so the track keeps playing after recovery.
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_reset_when_idle_is_harmless(console):
    """#1024: reset with no active playback is a valid no-op, not an error."""
    assert console.send_line("PLAY STOP") == ""
    assert console.send_line("PLAY RESET") == ""
    assert console.send_line("PRINT PLAYING()") == "0"
    assert console.send_line("PLAY TONE 440, 440, 40") == ""
    console.send_line("PAUSE 200")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_reset_clears_mix_counters(console):
    """#1024: reset restarts the MIXGAP/UNDERRUN window."""
    assert console.send_line('PLAY S3M "tests/TEST.S3M"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PAUSE 200")
    assert console.send_line("PLAY RESET") == ""
    gap = int(console.send_line('PRINT MM.INFO("MIXGAP")'))
    assert gap < 200
    assert console.send_line('PRINT MM.INFO("UNDERRUN")') == "0"
    assert console.send_line("PRINT PLAYING()") == "1"
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
