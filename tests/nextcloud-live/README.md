# Live Nextcloud Notes API test

This is a **standalone CMake/QtTest harness** for the real
`plugins/nextcloud/nextcloudworker.cpp`. It deliberately does not mock HTTP,
and avoids building the entire AnyKeep application or unrelated dependencies.
The authoritative storage-level fixture tests are still in
`tests/nextcloudstorage_test.cpp` and `tests/nextcloudworker_test.cpp`.

The GitHub Actions workflow at `.github/workflows/nextcloud-notes.yml` starts
the official `nextcloud:32-apache` Docker image using disposable SQLite storage,
installs the official **Notes** app with `occ app:install notes`, builds this
test and executes it against `http://127.0.0.1:8080`.

## What is exercised

- Notes API reachability and Basic authentication; invalid credentials rejected.
- Create -> list (without content) -> fetch full note.
- Category-only move, preserving note content.
- Update full note, including favorite, and read back the server state.
- Reusing a stale ETag must return HTTP 412 with the current remote note;
  the server's winning content must remain unchanged.
- Delete, then verify the note is absent.

Notes use unique IDs and best-effort scoped cleanup, so a failed assertion
does not contaminate subsequent local runs. The CI container is destroyed on
completion in all cases.

## Run locally

Start a dedicated disposable instance, then install Notes. For example:

```sh
docker run -d --rm --name anykeep-notes-ci -p 127.0.0.1:8080:80 \
  -e SQLITE_DATABASE=nextcloud \
  -e NEXTCLOUD_ADMIN_USER=anykeep-ci \
  -e NEXTCLOUD_ADMIN_PASSWORD=anykeep-local-ci-only \
  -e NEXTCLOUD_TRUSTED_DOMAINS='127.0.0.1 localhost' \
  nextcloud:32-apache
# After the server finishes its initial setup:
docker exec -u www-data anykeep-notes-ci php occ app:install notes

cmake -S tests/nextcloud-live -B build/nextcloud-live -G Ninja
cmake --build build/nextcloud-live

export NEXTCLOUD_TEST_URL=http://127.0.0.1:8080
export NEXTCLOUD_TEST_USER=anykeep-ci
export NEXTCLOUD_TEST_PASSWORD=anykeep-local-ci-only
ctest --test-dir build/nextcloud-live --output-on-failure

docker rm -f anykeep-notes-ci
```

**Do not aim this destructive test at a personal or production account.**
It creates, modifies, and deletes remote notes. The local HTTP URL and
nonsecret test password are suitable only for an isolated loopback server;
use HTTPS and real app passwords for actual deployments.

This verifies the plugin's HTTP/codec layer against a real server. It does not
yet cover the full AnyKeep `NoteStorage` job/cache lifecycle, Settings UI,
large pagination fixtures, or multi-user read-only sharing.
