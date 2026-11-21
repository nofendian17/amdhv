# Docker Desktop on AMDHV

Docker Desktop 4.100.0 runs its LinuxKit VM through macOS **Virtualization.framework** on this
host. `/Applications/Docker.app` carries `com.apple.security.virtualization`, so its `hv_vmx_*`
calls go through AMDHV (`amdhv_hv=1`, `kern.hv_support=1`).

**Not an officially supported client.** Verified on the reference host: the engine starts and an
nginx container answered HTTP 200 (`nginx/1.31.9`).

## 1. Check AMDHV is up

```bash
codesign -d --entitlements - /Applications/Docker.app 2>&1 | grep virtualization
sysctl kern.hv_support                 # must be 1
sudo build/amd-info  # SVM must be "active on" every CPU
```

Start Docker Desktop **after** boot. See the [README Install section](../README.md#install) if
`kern.hv_support` is 0.

## 2. Use the right Docker CLI

On this machine `docker` in PATH resolved to `~/.kiri/bin/docker` (Kiri's CLI). That produced
`docker-credential-desktop: executable file not found`. Use Docker Desktop's CLI explicitly:

```bash
~/.docker/bin/docker info --format '{{.ServerVersion}}'
```

Or put it first in PATH for the session:

```bash
export PATH="$HOME/.docker/bin:$PATH"
```

## 3. End-to-end check

```bash
~/.docker/bin/docker run --rm -d -p 8080:80 --name amdhv-nginx nginx
curl -sI http://localhost:8080 | head -1     # HTTP/1.1 200 OK
~/.docker/bin/docker rm -f amdhv-nginx
```

## 4. Verify it runs through AMDHV

```bash
sudo build/amd-info stats  # vmCreates/vmExits grow while the VM runs
                                             # vmInvalidExits and assertFailures must stay 0
```

## Troubleshooting

| symptom | cause and fix |
|---|---|
| "stuck starting the engine" | check `sysctl kern.hv_support` and AMDHV boot-args; the LinuxKit guest must boot before the engine comes up |
| `docker-credential-desktop: executable file not found` | `docker` in PATH is Kiri's CLI; use `~/.docker/bin/docker` or fix PATH |
| `kern.hv_support` reads 0 | AMDHV not loaded: check Kernel → Add in `config.plist`, boot-args `amdhv_enable_svm=1 amdhv_hv=1`, and `sudo build/amd-info log` |
