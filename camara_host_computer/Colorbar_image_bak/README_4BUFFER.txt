Colorbar_image four-buffer host

The driver allocates four coherent 4 MiB DMA buffers below 4 GiB.  It
programs all four addresses into PCIE_DMA_single_5 protocol v0x0B and
rotates frames through buffer indexes 0, 1, 2 and 3.

Build this source on LubanCat before use; the pre-existing build/ binaries
and driver/colorbar_pcie_driver.ko are not updated by editing the source.

Typical commands on LubanCat:
  make clean
  sudo scripts/load_driver.sh allow_dma_start=1
  ./build/pcie_color_gui

The log for continuous capture should show completed buffer indexes in the
order 0, 1, 2, 3, 0.  Each successful frame must report 4,147,200 bytes.
