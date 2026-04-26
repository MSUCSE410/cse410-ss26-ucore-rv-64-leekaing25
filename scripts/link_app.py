from pathlib import Path


OUT_PATH = Path("os/link_app.S")
APP_DIR = Path("user/target/bin")


def emit_app_table(apps: list[str]) -> str:
    lines: list[str] = []
    lines.append("    .align 4")
    lines.append("    .section .data")
    lines.append("    .global _app_num")
    lines.append("_app_num:")
    lines.append(f"    .quad {len(apps)}")
    for index in range(len(apps)):
        lines.append(f"    .quad app_{index}_start")
    lines.append(f"    .quad app_{len(apps) - 1}_end")
    lines.append("")
    lines.append("    .global _app_names")
    lines.append("_app_names:")
    for app in apps:
        lines.append(f'    .string "{app}"')
    lines.append("")
    for index, app in enumerate(apps):
        lines.append(f"    .section .data.app{index}")
        lines.append(f"    .global app_{index}_start")
        lines.append(f"app_{index}_start:")
        lines.append(f'    .incbin "./{APP_DIR}/{app}"')
        if index == len(apps) - 1:
            lines.append(f"app_{index}_end:")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    if not APP_DIR.is_dir():
        raise SystemExit(f"missing app directory: {APP_DIR}")
    apps = sorted(
        entry.name for entry in APP_DIR.iterdir()
        if entry.is_file() and not entry.name.startswith(".")
    )
    if not apps:
        raise SystemExit(f"no apps found in {APP_DIR}")
    OUT_PATH.write_text(emit_app_table(apps))


if __name__ == "__main__":
    main()
