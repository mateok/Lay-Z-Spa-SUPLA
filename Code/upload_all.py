Import("env", "projenv")

import sys

def after_firmware_upload(source, target, env):
    # Do not run a script when external applications, such as IDEs,
    # dump integration data. Otherwise, input() will block the process
    # waiting for the user input
    if env.IsIntegrationDump():
        # stop the current script execution
        return

    # IDE uploads and other non-interactive invocations have no stdin. Calling
    # input() there used to raise EOFError after the firmware had already been
    # flashed successfully, which made PlatformIO report a misleading failure.
    # Keep filesystem flashing explicit in that case because it replaces all
    # LittleFS data on the device.
    if not sys.stdin.isatty():
        print(
            "Firmware upload complete. Filesystem was not uploaded "
            "automatically; run 'pio run -e %s -t uploadfs' when needed."
            % env.get("PIOENV")
        )
        return
    
    print("\n=== Filesystem build and upload ===")
    
    # Prompt user for choice
    while True:
        print("\nDo you want to build and upload the filesystem image? (Y/n): ")
        try:
            choice = input().strip()
        except EOFError:
            print("No interactive input available; skipping filesystem upload.")
            return
        
        # Default to No if empty input
        if choice == "":
            choice = "N"
        
        # Convert to uppercase for comparison
        choice = choice.upper()
        
        if choice in ["Y", "YES"]:
            print("\n=== Building filesystem image ===")
            projenv.Execute("pio run --target buildfs --environment " + env.get("PIOENV"))
            print("\n=== Uploading filesystem image ===")
            projenv.Execute("pio run --target uploadfs --environment " + env.get("PIOENV"))
            break
        elif choice in ["N", "NO"]:
            print("Skipping filesystem build and upload.")
            break
        else:
            print("Please enter Y (yes) or N (no).")

env.AddPostAction("upload", after_firmware_upload)
