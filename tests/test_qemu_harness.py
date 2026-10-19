"""Host-side unit tests for the QEMU harness serial-handshake logic (#1211).

These need no QEMU: they drive the pure byte-buffer decision of when a
``send_line``/``send_keys`` response is complete. The console echoes the typed
command, so a ``>`` inside it (the ``<>`` not-equal operator, ``IF x > y``)
must not be mistaken for the ``> `` prompt.
"""

from harness.qemu_harness import (
    MMBasicConsole,
    _is_prompt_line,
    prompt_at_buffer_end,
)

PROMPT = b"> "


def _first_prompt_chunk(chunks, prompt=PROMPT):
    """Feed ``chunks`` in order and return the index of the first one that
    completes the response (or ``None``)."""
    buf = b""
    for i, chunk in enumerate(chunks):
        buf += chunk
        if prompt_at_buffer_end(buf, prompt):
            return i
    return None


def _extract(raw, echoed):
    return MMBasicConsole.__new__(MMBasicConsole)._extract_response(raw, echoed)


def test_gt_inside_echo_is_not_a_prompt():
    assert prompt_at_buffer_end(b"110 IF VAL(A$) <>", PROMPT) is False
    assert (
        prompt_at_buffer_end(
            b'110 IF VAL(A$) <> N THEN PRINT "BAD "; N; " "; A$', PROMPT
        )
        is False
    )
    assert prompt_at_buffer_end(b"IF x > y THEN PRINT 1", PROMPT) is False
    assert prompt_at_buffer_end(b'PRINT ">"', PROMPT) is False


def test_prompt_on_its_own_line_is_detected():
    assert prompt_at_buffer_end(b"NEW\r\r\n> ", PROMPT) is True
    assert prompt_at_buffer_end(b"PRINT 1+1\r\r\n2\r\n\r\n> ", PROMPT) is True
    # OPTION PROMPT CWD: the whole A:/> line is the prompt.
    assert prompt_at_buffer_end(b"CLS\r\r\nA:/> ", PROMPT) is True
    assert prompt_at_buffer_end(b"PRINT 1\r\r\n1\r\n\r\nA:/> ", PROMPT) is True


def test_empty_buffer_and_partial_echo_are_not_prompts():
    assert prompt_at_buffer_end(b"", PROMPT) is False
    assert prompt_at_buffer_end(b"PRINT 1\r", PROMPT) is False
    assert prompt_at_buffer_end(b"PRINT 1\r\r\n2\r\n", PROMPT) is False


def test_streaming_waits_for_the_real_prompt():
    """The regression from #1211: a chunk ending at the ``<>`` in the echo
    must not be taken for the prompt."""
    chunks = [
        b"110 IF VAL(A$) <>",
        b' N THEN PRINT "BAD "; N; " "; A$\r',
        b"\r\n",
        b"> ",
    ]
    assert _first_prompt_chunk(chunks) == len(chunks) - 1


def test_streaming_normal_response_completes_at_prompt():
    chunks = [b"PRINT 1+1\r", b"\r\n2\r\n", b"\r\n", b"> "]
    assert _first_prompt_chunk(chunks) == len(chunks) - 1


def test_is_prompt_line():
    assert _is_prompt_line(">") is True
    assert _is_prompt_line("A:/>") is True
    assert _is_prompt_line("A:/DIR/>") is True
    assert _is_prompt_line("110 IF VAL(A$) <>") is False
    assert _is_prompt_line("2") is False


def test_extract_response_handles_gt_in_command():
    line = '110 IF VAL(A$) <> N THEN PRINT "BAD "; N; " "; A$'
    raw = line.encode() + b"\r\r\n> "
    assert _extract(raw, line) == ""


def test_extract_response_normal():
    assert _extract(b"PRINT 1+1\r\r\n2\r\n\r\n> ", "PRINT 1+1") == "2"
    assert _extract(b"NEW\r\r\n> ", "NEW") == ""
    assert _extract(b"PRINT 1\r\r\n1\r\n\r\nA:/> ", "PRINT 1") == "1"
