#!/usr/bin/env python3

import pathlib
import sys
import tomlkit


def find_modules(top_dir, port_dir):
    """Find all available modules in shared-bindings and port bindings."""
    modules = set()
    for module in sorted(
        list(top_dir.glob("shared-bindings/*")) + list(port_dir.glob("bindings/*")),
        key=lambda x: x.name,
    ):
        if not module.is_dir():
            continue
        modules.add(module.name)
    return sorted(modules)


def find_board_info_files(port_dir):
    """Find all autogen_board_info.toml files in the port directory."""
    return list(port_dir.glob("boards/**/autogen_board_info.toml"))


def update_board_info(board_info_path, available_modules):
    """Update board info file with new modules set to false."""
    if not board_info_path.exists():
        print(f"Error: Board info file {board_info_path} does not exist", file=sys.stderr)
        return False

    # Load existing board info
    with open(board_info_path, "r", encoding="utf-8") as f:
        board_info = tomlkit.load(f)

    # Get current modules
    current_modules = set(board_info.get("modules", {}))

    # Find new modules
    new_modules = set(available_modules) - current_modules
    if not new_modules:
        print(
            f"No new modules found for {board_info_path.relative_to(board_info_path.parents[3])}"
        )
        return True

    # Add new modules as disabled in alphabetical order
    modules_table = board_info["modules"]
    # Get all modules (existing and new) and sort them
    all_modules = list(current_modules | new_modules)
    all_modules.sort()

    # Create a new table with sorted modules
    sorted_table = tomlkit.table()
    for module in all_modules:
        if module in modules_table:
            # TODO: Use modules_table.item once tomlkit is released with changes from January 2025
            sorted_table[module] = modules_table._value.item(module)
        else:
            sorted_table[module] = tomlkit.item(False)

    # Replace the modules table with the sorted one
    board_info["modules"] = sorted_table

    # Write updated board info
    with open(board_info_path, "w", encoding="utf-8") as f:
        tomlkit.dump(board_info, f)

    print(
        f"Updated {board_info_path.relative_to(board_info_path.parents[3])} with {len(new_modules)} new modules:"
    )
    for module in sorted(new_modules):
        print(f"  - {module}")
    return True


def main():
    # Get repo paths
    script_dir = pathlib.Path(__file__).parent
    top_dir = script_dir.parents[2]  # circuitpython root
    port_dir = script_dir.parent  # zephyr-cp directory

    # Get available modules once
    available_modules = find_modules(top_dir, port_dir)

    # Update all board info files
    board_info_files = find_board_info_files(port_dir)
    if not board_info_files:
        print("No board info files found")
        sys.exit(1)

    success = True
    for board_info_path in board_info_files:
        if not update_board_info(board_info_path, available_modules):
            success = False

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
