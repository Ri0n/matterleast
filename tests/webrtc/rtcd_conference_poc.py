#!/usr/bin/python3
"""Three independent webrtcbin participants in one RTCD audio conference.

Alice and Bob first establish a working two-party call. Charlie joins later,
while both are sending and receiving Opus. After RTCD renegotiates, every
participant must decode at least 10 buffers on EACH of two distinct incoming
RTP pads. Alice and Bob must also keep decoding the original remote track
after Charlie joins, not just receive a third stream.

This is deliberately RTCD's internal test API, not a public Calls plugin API.
"""
import base64
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

from rtcd_webrtcbin_poc import RTCD_URL


CLIENT_ID = "matterleast-ci-conference"
RTCD_CONTAINER = "matterleast-rtcd-poc"
PHASE_TIMEOUT = 40


def log(message):
    print("[conference] " + message, flush=True)


def register_once():
    key = secrets.token_urlsafe(36)
    request = urllib.request.Request(
        RTCD_URL + "/register",
        data=json.dumps({"clientID": CLIENT_ID, "authKey": key}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        if response.status != 201:
            raise RuntimeError("RTCD registration failed: " + str(response.status))
    return key


class Participant:
    def __init__(self, name, shared_key, state_dir):
        self.name = name
        self.state_dir = state_dir
        self.output = []
        env = dict(os.environ)
        env.update({
            "RTCD_AUTH_KEY": shared_key,
            "RTCD_CLIENT_ID": CLIENT_ID,
            "RTCD_PARTICIPANT": name,
            "RTCD_STATE_DIR": str(state_dir),
        })
        self.process = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("rtcd_webrtcbin_poc.py"))],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
        threading.Thread(target=self.read_output, daemon=True).start()
        log("started " + name + " (PID " + str(self.process.pid) + ")")

    def read_output(self):
        for line in self.process.stdout:
            self.output.append(line)
            print("[" + self.name + "] " + line, end="", flush=True)

    def marker(self, state):
        return self.state_dir / (self.name + "-" + state)

    def signal(self, state):
        self.marker(state).touch()
        log(self.name + ": requested " + state)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def wait_for(description, predicate, participants, timeout=PHASE_TIMEOUT):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for participant in participants:
            rc = participant.process.poll()
            if rc is not None:
                raise RuntimeError(
                    "Participant %s exited (%d) during %s" %
                    (participant.name, rc, description)
                )
        if predicate():
            log("confirmed " + description)
            return
        time.sleep(0.1)
    raise TimeoutError("Timed out waiting for " + description)


def sfu_has_track(name):
    result = subprocess.run(
        ["docker", "logs", RTCD_CONTAINER],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=True,
    )
    session_id = "sessionID=matterleast-poc-session-" + name
    return any(
        "new track received" in line and session_id in line
        for line in result.stdout.splitlines()
    )


def main():
    participants = []
    try:
        with tempfile.TemporaryDirectory(prefix="rtcd-conference-") as dirname:
            state_dir = Path(dirname)
            key = register_once()
            log("registered one RTCD group; verifying 2-party audio before "
                "a third participant joins late")
            alice = Participant("alice", key, state_dir)
            participants.append(alice)

            wait_for("Alice DataChannel", lambda: alice.marker("dc-ready").exists(), participants)
            alice.signal("publish")
            wait_for("Alice local audio negotiation", lambda: alice.marker("publishing").exists(), participants)
            wait_for("Alice incoming Opus at RTCD", lambda: sfu_has_track("alice"), participants)

            # Bob arrives after Alice is already publishing. This tests
            # RTCD's existing-track forwarding on conference late join.
            bob = Participant("bob", key, state_dir)
            participants.append(bob)
            wait_for("Bob DataChannel", lambda: bob.marker("dc-ready").exists(), participants)
            wait_for("Bob decoded Alice Opus through SFU",
                     lambda: bob.marker("remote-1").exists(), participants)

            # Bob publishes after his incoming SFU offer was answered,
            # avoiding unnecessary SDP glare while testing both directions.
            bob.signal("publish")
            wait_for("Bob local audio negotiation", lambda: bob.marker("publishing").exists(), participants)
            wait_for("Bob incoming Opus at RTCD", lambda: sfu_has_track("bob"), participants)
            wait_for("Alice decoded Bob Opus through SFU",
                     lambda: alice.marker("remote-1").exists(), participants)
            log("2-party audio verified in both directions; joining Charlie late")

            # This is a genuine late join: Alice and Bob have already
            # decoded the other's audio before Charlie creates his own
            # WebSocket, PeerConnection and Opus sender.
            charlie = Participant("charlie", key, state_dir)
            participants.append(charlie)
            wait_for("Charlie DataChannel",
                     lambda: charlie.marker("dc-ready").exists(), participants)
            charlie.signal("publish")
            wait_for("Charlie local audio negotiation",
                     lambda: charlie.marker("publishing").exists(), participants)
            wait_for("Charlie incoming Opus at RTCD",
                     lambda: sfu_has_track("charlie"), participants)

            # A marker is written only after 10 *decoded* PCM buffers per
            # separate incoming RTP pad, so these cannot be satisfied by
            # receiving 20 packets from only one remote peer.
            for recipient in (charlie, alice, bob):
                wait_for("%s decoded two distinct remote Opus tracks" % recipient.name,
                         lambda recipient=recipient:
                         recipient.marker("remote-2").exists(), participants)

            # Verify that the original Alice↔Bob audio survives the new
            # conference member and the server-initiated renegotiations.
            for original in (alice, bob):
                original.signal("check-continuity")
            for original in (alice, bob):
                wait_for("%s armed original-track continuity" % original.name,
                         lambda original=original:
                         original.marker("continuity-armed").exists(), participants)
                wait_for("%s continued original audio after late join" % original.name,
                         lambda original=original:
                         original.marker("original-continued").exists(), participants)

            for participant in participants:
                participant.signal("finish")
            for participant in participants:
                rc = participant.process.wait(timeout=10)
                if rc != 0:
                    raise RuntimeError("Participant %s exited with %d" % (participant.name, rc))
                if not any("PASS conference " + participant.name in line
                           for line in participant.output):
                    raise RuntimeError("Missing successful conference result for " + participant.name)
                if not any("answered RTCD-initiated offer" in line
                           for line in participant.output):
                    raise RuntimeError("RTCD never renegotiated incoming audio for " + participant.name)
                if not any("2 independent remote Opus tracks" in line
                           for line in participant.output):
                    raise RuntimeError("Missing two independently decoded tracks for " + participant.name)
            log("PASS: three independent webrtcbin peers each decoded two "
                "remote Opus tracks through RTCD SFU; Charlie joined after "
                "two-way Alice/Bob audio and original tracks kept flowing")
            return 0
    except Exception as exc:
        log("FAIL: " + repr(exc))
        return 1
    finally:
        for participant in participants:
            participant.stop()


if __name__ == "__main__":
    sys.exit(main())
