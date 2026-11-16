#!/usr/bin/env python3
"""Integration test for lmcurl LDAPv3 client (both direct CLI and _lmcode_ script engine) against lmldap."""
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build" / "lmcurl").resolve()
COMPILER = sys.argv[2] if len(sys.argv) > 2 else shutil.which("lmscript")


def find_lmldap():
    if os.environ.get("LMLDAP_BIN") and Path(os.environ["LMLDAP_BIN"]).is_file():
        return os.environ["LMLDAP_BIN"]
    sibling = ROOT.parent / "lmldap" / "lmldap"
    if sibling.is_file():
        return str(sibling)
    found = shutil.which("lmldap")
    if found:
        return found
    return None


def run_cli(*args):
    r = subprocess.run([str(HOST), *map(str, args)], capture_output=True, text=True, timeout=15)
    assert r.returncode == 0, f"CLI failed ({r.returncode}): {args}\nstdout={r.stdout}\nstderr={r.stderr}"
    return json.loads(r.stdout.strip())


def main():
    lmldap = find_lmldap()
    if not lmldap:
        if os.environ.get("LMCURL_REQUIRE_LMLDAP") == "1":
            raise SystemExit("ERROR: LMLDAP_BIN required when LMCURL_REQUIRE_LMLDAP=1")
        print("SKIP LOUDLY: test-ldap.py requires lmldap on PATH or ../lmldap/lmldap")
        return

    ldap_port = random.randint(25000, 26000)
    rest_port = ldap_port + 1000
    admin_dn = "cn=admin,dc=example,dc=com"
    admin_pw = "secret"

    with tempfile.TemporaryDirectory(prefix="lmcurl-ldap-") as tmpdir:
        srv = subprocess.Popen(
            [
                lmldap,
                "--port", str(ldap_port),
                "--rest-port", str(rest_port),
                "--base-dn", "dc=example,dc=com",
                "--admin-dn", admin_dn,
                "--admin-pw", admin_pw,
                "--data-dir", tmpdir,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            ready = False
            for _ in range(60):
                r = subprocess.run(
                    [str(HOST), f"ldap://127.0.0.1:{ldap_port}/ping"],
                    capture_output=True, text=True,
                )
                if r.returncode == 0 and '"PONG"' in r.stdout:
                    ready = True
                    break
                time.sleep(0.1)
            assert ready, "lmldap failed to start within timeout"

            # 1. Direct CLI URL mode & `lmcurl ldap` subcommands
            assert run_cli(f"ldap://127.0.0.1:{ldap_port}/ping")["result"] == "PONG"
            assert run_cli("ldap", "ping", "--port", ldap_port)["result"] == "PONG"
            assert run_cli("ldap", "bind", "--port", ldap_port, "--dn", admin_dn, "--password", admin_pw)["result"]["bound"] is True

            whoami = run_cli("ldap", "whoami", "--port", ldap_port, "--bind-dn", admin_dn, "--bind-pw", admin_pw)
            assert "cn=admin,dc=example,dc=com" in whoami["result"]["authzId"].lower(), whoami

            alice_dn = "cn=Alice,dc=example,dc=com"
            alice_attrs = json.dumps({
                "objectClass": ["top", "person", "inetOrgPerson"],
                "cn": "Alice",
                "sn": "Smith",
                "mail": "alice@example.com",
            })
            assert run_cli(
                "ldap", "add", alice_dn, alice_attrs,
                "--port", ldap_port, "--bind-dn", admin_dn, "--bind-pw", admin_pw,
            )["result"] == "OK"

            bob_dn = "cn=Bob,dc=example,dc=com"
            bob_attrs = json.dumps({
                "objectClass": ["top", "person", "inetOrgPerson"],
                "cn": "Bob",
                "sn": "Jones",
                "mail": "bob@example.com",
            })
            assert run_cli(
                "-u", f"{admin_dn}:{admin_pw}",
                "-X", "PUT",
                "-d", bob_attrs,
                f"ldap://127.0.0.1:{ldap_port}/entry/{bob_dn}",
            )["result"] == "OK"

            # RFC 4516 search with attribute projection
            search_res = run_cli(f"ldap://127.0.0.1:{ldap_port}/dc=example,dc=com?cn,mail?sub?(cn=Alice)")
            assert search_res["count"] == 1, search_res
            assert search_res["entries"][0]["attributes"]["mail"] == "alice@example.com"

            # Compare operation
            cmp_true = run_cli("ldap", "compare", alice_dn, "sn", "Smith", "--port", ldap_port)
            assert cmp_true["result"]["match"] is True
            cmp_false = run_cli("ldap", "compare", alice_dn, "sn", "Jones", "--port", ldap_port)
            assert cmp_false["result"]["match"] is False

            # Modify operation
            assert run_cli(
                "ldap", "modify", alice_dn, json.dumps({"replace": {"mail": "alice.new@example.com"}}),
                "--port", ldap_port, "--bind-dn", admin_dn, "--bind-pw", admin_pw,
            )["result"] == "OK"

            get_res = run_cli("ldap", "get", alice_dn, "--port", ldap_port)
            assert get_res["result"]["attributes"]["mail"] == "alice.new@example.com"

            assert run_cli(
                "ldap", "del", bob_dn,
                "--port", ldap_port, "--bind-dn", admin_dn, "--bind-pw", admin_pw,
            )["result"] == 1

            # 2. _lmcode_ script engine mode
            if COMPILER:
                lms_path = Path(tmpdir) / "ldap_engine.lms"
                lms_path.write_text(
                    f's = load_schema(cli="{HOST}");\n'
                    f'p = _lmcode_ s;\n'
                    f'let l = curl.session("ldap://127.0.0.1:{ldap_port}");\n'
                    f'let r_ping = l.get("/ping").send();\n'
                    f'assert_eq(r_ping.status(), 200);\n'
                    f'assert_eq(r_ping.json("$.result").value(), "PONG");\n'
                    f'let r_who = l.get("/whoami").basic("{admin_dn}", "{admin_pw}").send();\n'
                    f'assert_eq(r_who.status(), 200);\n'
                    f'let r_cmp = l.get("/compare?dn=cn%3DAlice%2Cdc%3Dexample%2Cdc%3Dcom&attr=sn&value=Smith").send();\n'
                    f'assert_eq(r_cmp.status(), 200);\n'
                    f'assert_eq(json_parse(r_cmp.body()).get("result").get("match").as_bool(), true);\n'
                    f'let r_search = l.get("/dc=example,dc=com?cn,mail?sub?(cn=Alice)").send();\n'
                    f'assert_eq(r_search.status(), 200);\n'
                    f'assert_eq(r_search.json("$.entries[0].attributes.mail").value(), "alice.new@example.com");\n'
                    f'end _lmcode_\n'
                    f'res = run_lmcode(p);\n'
                    f'require(res.ok, json_stringify(res));\n'
                )
                env = dict(os.environ, PATH=f"{HOST.parent}{os.pathsep}{os.environ.get('PATH', '')}")
                eng = subprocess.run([COMPILER, str(lms_path)], env=env, capture_output=True, text=True, timeout=30)
                assert eng.returncode == 0, f"_lmcode_ LDAP test failed:\nstdout={eng.stdout}\nstderr={eng.stderr}"

            print("PASS: lmcurl LDAPv3 CLI (URL mode + ldap subcommands) and _lmcode_ script engine against live lmldap")
        finally:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()


if __name__ == "__main__":
    main()
