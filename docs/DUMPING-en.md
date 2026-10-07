// Adapted from github.com/hedge-dev/UnleashedRecomp

# Dumping

> [!NOTE]
> The following guide is for dumping the game from an Xbox 360 hard drive.
>
> If you wish to use a USB storage device, use [this guide](DUMPING-USB-en.md) instead.

### Pre-requisites
- Xbox 360 (modifications not necessary)
- Xbox 360 Hard Drive (20 GB minimum)
- Xbox 360 Hard Drive Transfer Cable (or a compatible SATA to USB adapter)
- Spider-Man: Edge of Time for Xbox 360 (US)
    - Retail Disc or Digital Copy (the Games on Demand release was delisted in 2014, so a digital copy has to be one you already own).
    - Title Update required.
    - The Identity Crisis Suits DLC is optional.
- [7-Zip](https://7-zip.org/download.html) (for extracting Velocity)
- [Velocity](https://github.com/Gualdimar/Velocity/releases/download/xex%2Biso-branch/Velocity-XEXISO.rar) (Gualdimar's fork)

> [!TIP]
> If you do not have the Xbox 360 Hard Drive Transfer Cable, make sure you buy the correct revision for your console.
>
> The latest revision works with both original Xbox 360 and Xbox 360 S|E hard drives, but the first revision only works with original Xbox 360 hard drives.
>
> To know which is which, the first revision cable is gray, whereas the latest revision (which supports any Xbox 360 hard drive) is black.

### Instructions

> [!NOTE]
> If you have a digital copy of Spider-Man: Edge of Time, skip to step 4.

1. Insert your retail disc copy of Spider-Man: Edge of Time into the Xbox 360 disc tray.
2. At the Xbox Dashboard, go over to the disc tile under the **home** tab and press X to view **Game Details**.
3. Under the **overview** tab, select the **Install** tile and choose to install to the primary hard drive.
4. Once installed, turn off your Xbox 360 and remove the hard drive from your console.

> [!TIP]
> You may consult the following guides if you're unsure on how to do this:
> - [Xbox 360](https://www.ifixit.com/Guide/Xbox+360+Hard+Drive+Replacement/3326)
> - [Xbox 360 S](https://www.ifixit.com/Guide/Xbox+360+S+Hard+Drive+Replacement/3184)
> - [Xbox 360 E](https://www.ifixit.com/Guide/Xbox+360+E+Hard+Drive+Replacement/22179)

5. Using the Xbox 360 Hard Drive Transfer Cable (or compatible SATA to USB adapter), connect your Xbox 360 hard drive to your PC.

> [!CAUTION]
> If you're using an unofficial SATA to USB adapter, you may need to remove the hard drive from its enclosure in order to connect it.
>
> For original Xbox 360 hard drives, this process is as simple as [removing some screws and cracking open the enclosure](https://www.ifixit.com/Guide/Xbox+360+HDD+Replacement/3430).
>
> For Xbox 360 S|E hard drives, this enclosure is glued shut and removing the hard drive may be an irreversible process!
>
> **It is highly recommended** that you obtain the official Xbox 360 Hard Drive Transfer Cable in order to proceed.

6. Download [the latest release of Velocity](https://github.com/Gualdimar/Velocity/releases/download/xex%2Biso-branch/Velocity-XEXISO.rar) and open the `*.rar` file using [7-Zip](https://7-zip.org/download.html), then extract its contents anywhere that's convenient to you.
7. Create a new folder anywhere that's convenient to you for storing the game files.

> [!NOTE]
> If you're using Linux, skip to step 9.

8. Right-click `Velocity.exe` and click **Properties**, then under the **Compatibility** tab, tick **Run this program as an administrator** and click **OK**. This is required for Velocity to recognize the hard drive.
9. Launch `Velocity.exe`. You should see a **Device Detected** message asking if you would like to open the **Device Content Viewer**. Click **Yes**.
10. You should now see a tree view of your hard drive's contents. Expand the tree nodes for `/Shared Items/Games/` (and `/Shared Items/DLC/`, if you have the DLC installed).
11. Hold the CTRL key and click on **Spider-Man: Edge of Time** under the `Games` node (Velocity may list it as **Spider-Man™: EoT**), as well as **Identity Crisis Suits** under the `DLC` node, if you have the DLC installed. Make sure everything is selected before the next step.
12. Right-click any of the selected items and click **Copy Selected to Local Disk**, then navigate to the folder you created in step 7 and select it. Velocity will now copy the game files to your PC.
13. Once the transfer is complete, close the **Device Content Viewer** window and navigate to **Tools > Device Tools > Raw Device Viewer**.
14. Navigate to `/Content/Cache/` and click the **Name** column to sort by name. Look for a file that begins with `TU_10LC25I`. This is the title update, which is required for installation.

> [!TIP]
> To check that this is the right file, double-click it to open it in **Package Viewer**. It should be named **Spider-Man Edge of Time Title Update #1**, with `Default.xexp` inside along with a folder called `Data` containing  `GameLogic.dllp`.
>
> Once you've checked it, close the **Package Viewer** window and continue to the next step.

15. Right-click the file that begins with `TU_10LC25I` and click **Copy Selected to Local Disk**, then navigate to the folder you created in step 7 and select it. Velocity will now copy the title update to your PC.
16. Once the transfer is complete, you should have all of the files needed for installation. [Return to the readme and continue to the next step](/README.md#how-to-install).
