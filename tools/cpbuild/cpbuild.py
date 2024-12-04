import asyncio
import inspect
import logging
import os
import pathlib
import shlex
import time
import atexit
import json
import re
import sys

logger = logging.getLogger(__name__)

shared_semaphore = None

trace_entries = []


def save_trace():
    with open("trace.json", "w") as f:
        json.dump(trace_entries, f)
    print("wrote trace", pathlib.Path(".").absolute() / "trace.json")


atexit.register(save_trace)


class _TokenProtocol(asyncio.Protocol):
    def __init__(self, client):
        self.client = client

    def data_received(self, data):
        # Data can be multiple tokens at once.
        for i, _ in enumerate(data):
            self.client.new_token(data[i : i + 1])


class _MakeJobClient:
    def __init__(self, fifo_path=None, read_fd=None, write_fd=None):
        self.fifo_path = fifo_path
        if fifo_path is not None:
            self.writer = open(fifo_path, "wb")
            self.reader = open(fifo_path, "rb")
        self.tokens_in_use = []
        self.pending_futures = []

        self.read_transport: asyncio.ReadTransport | None = None
        self.read_protocol = None

    def new_token(self, token):
        # Keep a token and reuse it. Ignore cancelled Futures.
        if self.pending_futures:
            future = self.pending_futures.pop(0)
            while future.cancelled() and self.pending_futures:
                future = self.pending_futures.pop(0)
            if not future.cancelled():
                future.set_result(token)
                return
        self.read_transport.pause_reading()
        self.writer.write(token)
        self.writer.flush()

    async def __aenter__(self):
        loop = asyncio.get_event_loop()
        if self.read_transport is None:
            self.read_transport, self.read_protocol = await loop.connect_read_pipe(
                lambda: _TokenProtocol(self), self.reader
            )
        future = loop.create_future()
        self.pending_futures.append(future)
        self.read_transport.resume_reading()
        self.tokens_in_use.append(await future)

    async def __aexit__(self, exc_type, exc, tb):
        token = self.tokens_in_use.pop()
        self.new_token(token)


def _create_semaphore():
    match = re.search(r"fifo:([^\s]+)", os.environ.get("MAKEFLAGS", ""))
    fifo_path = None
    if match:
        fifo_path = match.group(1)
        return _MakeJobClient(fifo_path=fifo_path)
    return asyncio.BoundedSemaphore(1)


shared_semaphore = _create_semaphore()
tracks = []
max_track = 0


async def run_command(command, working_directory, description=None):
    if isinstance(command, list):
        for i, part in enumerate(command):
            if isinstance(part, pathlib.Path):
                part = part.relative_to(working_directory, walk_up=True)
            # if isinstance(part, list):

            command[i] = str(part)
        command = " ".join(command)

    cancellation = None
    async with shared_semaphore:
        global max_track
        if not tracks:
            max_track += 1
            tracks.append(max_track)
        track = tracks.pop()
        start_time = time.perf_counter_ns() // 1000
        process = await asyncio.create_subprocess_shell(
            command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=working_directory,
        )

        try:
            stdout, stderr = await process.communicate()
        except asyncio.CancelledError as e:
            cancellation = e
            stdout, stderr = await process.communicate()
        end_time = time.perf_counter_ns() // 1000
        trace_entries.append(
            {
                "name": command if not description else description,
                "ph": "X",
                "pid": 0,
                "tid": track,
                "ts": start_time,
                "dur": end_time - start_time,
            }
        )
        tracks.append(track)

    if process.returncode == 0:
        # If something has failed and we've been canceled, hide our success so
        # the error is clear.
        if cancellation:
            raise cancellation
        if description:
            logger.info(description)
            logger.debug(command)
        else:
            logger.info(command)
    else:
        if stdout:
            logger.info(stdout.decode("utf-8").strip())
        if stderr:
            logger.warning(stderr.decode("utf-8").strip())
        if not stdout and not stderr:
            logger.warning("No output")
        logger.error(command)
        if cancellation:
            raise cancellation
        raise RuntimeError()


async def run_function(
    function,
    positional,
    named,
    description=None,
):
    async with shared_semaphore:
        global max_track
        if not tracks:
            max_track += 1
            tracks.append(max_track)
        track = tracks.pop()
        start_time = time.perf_counter_ns() // 1000
        result = await asyncio.to_thread(function, *positional, **named)

        end_time = time.perf_counter_ns() // 1000
        trace_entries.append(
            {
                "name": str(function) if not description else description,
                "ph": "X",
                "pid": 0,
                "tid": track,
                "ts": start_time,
                "dur": end_time - start_time,
            }
        )
        tracks.append(track)

    if description:
        logger.info(description)
        logger.debug(function)
    else:
        logger.info(function)
    return result


def run_in_thread(function):
    def wrapper(*positional, **named):
        return run_function(function, positional, named)

    return wrapper


cwd = pathlib.Path.cwd()


class Compiler:
    def __init__(self, srcdir: pathlib.Path, builddir: pathlib.Path, cmake_args):
        self.c_compiler = cmake_args["CC"]
        self.ar = cmake_args["AR"]
        self.cflags = cmake_args.get("CFLAGS", "")

        self.srcdir = srcdir
        self.builddir = builddir

    async def preprocess(
        self, source_file: pathlib.Path, output_file: pathlib.Path, flags: list[pathlib.Path]
    ):
        output_file.parent.mkdir(parents=True, exist_ok=True)
        await run_command(
            [
                self.c_compiler,
                "-E",
                "-MMD",
                "-c",
                source_file,
                self.cflags,
                *flags,
                "-o",
                output_file,
            ],
            description=f"Preprocess {source_file.relative_to(self.srcdir)} -> {output_file.relative_to(self.builddir)}",
            working_directory=self.srcdir,
        )

    async def compile(
        self, source_file: pathlib.Path, output_file: pathlib.Path, flags: list[pathlib.Path] = []
    ):
        if isinstance(source_file, str):
            source_file = self.srcdir / source_file
        if isinstance(output_file, str):
            output_file = self.builddir / output_file
        output_file.parent.mkdir(parents=True, exist_ok=True)
        await run_command(
            [self.c_compiler, self.cflags, "-MMD", "-c", source_file, *flags, "-o", output_file],
            description=f"Compile {source_file.relative_to(self.srcdir)} -> {output_file.relative_to(self.builddir)}",
            working_directory=self.srcdir,
        )

    async def archive(self, objects: list[pathlib.Path], output_file: pathlib.Path):
        output_file.parent.mkdir(parents=True, exist_ok=True)
        await run_command(
            [self.ar, "rvs", output_file, *objects],
            description=f"Create archive {output_file.relative_to(self.srcdir)}",
            working_directory=self.srcdir,
        )

    async def link(
        self,
        objects: list[pathlib.Path],
        output_file: pathlib.Path,
        linker_script: pathlib.Path,
        flags: list[str] = [],
        print_memory_use=True,
        output_map_file=True,
        gc_sections=True,
    ):
        output_file.parent.mkdir(parents=True, exist_ok=True)
        link_flags = []
        if print_memory_use:
            link_flags.append("-Wl,--print-memory-usage")
        if output_map_file:
            link_flags.append(
                "-Wl,-Map="
                + str(output_file.with_suffix(".elf.map").relative_to(caller_directory))
            )
        if gc_sections:
            link_flags.append("-Wl,--gc-sections")
        await run_command(
            [
                self.c_compiler,
                *link_flags,
                *flags,
                *objects,
                "-fuse-ld=lld",
                "-T",
                linker_script,
                "-o",
                output_file,
            ],
            description=f"Link {output_file.relative_to(cwd)}",
            working_directory=caller_directory,
        )
