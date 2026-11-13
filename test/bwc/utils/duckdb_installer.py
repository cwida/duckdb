import os
import subprocess
import tempfile
from pathlib import Path
import urllib.request
from utils.logger import make_logger

from .duckdb_cli import DuckDBCLI

logger = make_logger(__name__)


def make_cli_path(version):
    raw_version = version.lstrip('v')
    cli_path = Path.home() / ".duckdb" / "cli" / raw_version / "duckdb"
    return str(cli_path)


def install_duckdb_cli(version):
    # Check if the cli already exists
    cli_path = make_cli_path(version)
    if os.path.exists(cli_path):
        logger.debug(f"DuckDB CLI {version} already exists at '{cli_path}'")
        return

    req = urllib.request.Request("https://install.duckdb.org", headers={"User-Agent": "duckdb-bwc-test-runner"})
    with urllib.request.urlopen(req) as response:
        script_content = response.read()

    env = os.environ.copy()
    env['DUCKDB_VERSION'] = version.lstrip('v')

    result = subprocess.run(
        ['sh'],
        input=script_content,
        env=env,
        capture_output=True,
        text=False,  # Keep as bytes since we're reading bytes
    )

    if result.returncode != 0:
        raise RuntimeError(f"[{version}] Install script failed: {result.stderr.decode()}")

    if not os.path.exists(cli_path):
        raise RuntimeError(
            f"[{version}] Install script succeeded but CLI not found at '{cli_path}'. stdout: {result.stdout.decode()}"
        )

    logger.info(f"[{version}] DuckDB CLI installed successfully at '{cli_path}'")


def download_test_specs(version, target_sub_dir):
    logger.debug(f"[{version}] Downloading DuckDB test specs")
    target_dir = os.path.join(target_sub_dir, version)
    if os.path.exists(target_dir):
        logger.info(f"[{version}] Test specs already exist at '{target_dir}'")
        return

    with tempfile.TemporaryDirectory() as tmpdirname:
        logger.info(f"[{version}] Cloning duckdb repo in {tmpdirname}")
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", version, f"https://github.com/duckdb/duckdb.git"],
            cwd=tmpdirname,
            check=True,
            capture_output=True,
            text=True,
        )
        logger.info(f"[{version}] Cloned duckdb repo.")

        # Move `test` and `data` directories to target_dir
        os.makedirs(target_dir)
        subprocess.run(["mv", f"duckdb/test", target_dir], cwd=tmpdirname, check=True)
        subprocess.run(["mv", f"duckdb/data", target_dir], cwd=tmpdirname, check=True)
        logger.info(f"[{version}] Copied test specs to '{target_dir}'")


def get_version(cli_path):
    with DuckDBCLI(cli_path) as cli:
        result = cli.execute_command("select library_version, source_id from pragma_version();")
        if not result["success"]:
            raise RuntimeError(f"Failed to get DuckDB version from CLI at '{cli_path}': {result['stderr']}")
        return result["output"][1].split(',')


def install_extensions(cli_path, extensions):
    with DuckDBCLI(cli_path, unsigned=True) as cli:
        installed = []
        failed = []

        # Happy path - all extensions installed
        ext_list = ", ".join(f"'{ext}'" for ext in extensions)
        result = cli.execute_command(
            f"SELECT COUNT(*) FROM duckdb_extensions() WHERE extension_name in ({ext_list}) AND installed;"
        )
        if result["output"][1] == str(len(extensions)):
            logger.info(f"[{cli.version}] All {len(extensions)} extensions were already installed")
            return

        for ext in extensions:
            # First look if it's already installed
            result = cli.execute_command(
                f"SELECT COUNT(*) FROM duckdb_extensions() WHERE extension_name='{ext}' AND installed;"
            )
            if result["output"][1] == '1':
                logger.debug(f"[{cli.version}] Extension '{ext}' is already installed.")
                installed.append(ext)
                continue

            if ext == 'test_utils':
                result = cli.execute_command(
                    f"INSTALL 'test_utils' FROM 'https://raw.githubusercontent.com/duckdb/bwc-test-utils/main/extensions';"
                )
            else:
                result = cli.execute_command(f"INSTALL '{ext}'")

            if result["success"]:
                installed.append(ext)
            else:
                failed.append(ext)

        logger.info(f"[{cli.version}] Installed {len(installed)} extensions.")

        if len(failed):
            root_ext_cmake_path = Path(__file__).parent.parent.parent.parent / ".github/config/extensions"
            if not root_ext_cmake_path.is_dir():
                raise RuntimeError(
                    f"Extensions CMake config directory does not exist at '{root_ext_cmake_path}' - did the repo structure change?"
                )

            # test if `.github/config/extensions/{extension}.cmake` is a file
            cmake_files_ext = []
            core_extensions = []
            for ext in failed:
                ext_filename = "test-utils" if ext == "test_utils" else ext
                ext_cmake_path = root_ext_cmake_path / f"{ext_filename}.cmake"
                if ext_cmake_path.is_file():
                    cmake_files_ext.append(ext_filename)
                else:
                    core_extensions.append(ext)

            # Build a string that suggests how to install the failed extensions
            install_cmd = f"make release "
            if len(cmake_files_ext) > 0:
                install_cmd += (
                    "EXTENSION_CONFIGS='"
                    + ";".join([f".github/config/extensions/{ext}.cmake" for ext in cmake_files_ext])
                    + "' "
                )
            install_cmd += "DUCKDB_EXTEND='" + ";".join(failed) + "'"

            if len(cmake_files_ext) > 0 or len(core_extensions) > 0:
                logger.info(
                    f"[{cli.version}] Failed to install {len(failed)} extensions. If you need to compile them:\n{install_cmd}"
                )
            raise RuntimeError(f"[{cli.version}] Failed to install {len(failed)} extensions: {', '.join(failed)}")


def install_assets(version, bwc_tests_base_dir):
    install_duckdb_cli(version)
    download_test_specs(version, f"{bwc_tests_base_dir}/specs")
