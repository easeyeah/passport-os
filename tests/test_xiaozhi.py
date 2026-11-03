"""Compile production Xiaozhi sources with deterministic fault-injection mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="passport-xiaozhi-") as temporary:
    # Apple clang++ 把 .c 输入当 C++ 编译会 -Werror;与 test_radio_catalog 一致,
    # cJSON 先用 cc 单独编译成 .o 再链接。
    json_obj = Path(temporary) / "json.o"
    subprocess.run(["cc", "-c", "-fsanitize=address,undefined",
                    "-Itests/vendor/cjson", "tests/vendor/cjson/cJSON.c",
                    "-o", str(json_obj)], cwd=ROOT, check=True)
    for name in ("app", "audio", "protocol"):
        executable = Path(temporary) / name
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-g", "-O1",
                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-Wno-unused-variable", "-Wno-misleading-indentation",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-Itests/xiaozhi_mocks", "-Imain", "-Icomponents/bsp/include",
                   f"tests/test_xiaozhi_{name}.cc",
                   "-o", str(executable)]
        if name == "protocol":
            command.append(str(json_obj))
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
print("Xiaozhi host regression: PASS (device tests not run)")
