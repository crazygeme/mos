#include <test/test.h>
#include <device/device.h>
#include <device/ps2.h>
#include <driver/driver.h>
#include <errno.h>

static unsigned probe_calls;
static int probe_result;

static int mock_probe(uint32_t address, uint16_t vendor, uint16_t device,
		      const pci_device_id *id)
{
	probe_calls++;
	return probe_result;
}

static const pci_device_id mock_ids[] = {
	{ .vendor_id = 0xfffe, .device_id = 0x1234 },
	{ .vendor_id = PCI_ANY_ID,
	  .device_id = PCI_ANY_ID,
	  .class_code = 0x0101,
	  .class_mask = 0xffff },
};

static driver_t mock_driver = {
	.name = "test-pci",
	.bus = DEVICE_BUS_PCI,
	.pci_ids = mock_ids,
	.pci_id_count = 2,
	.probe_pci = mock_probe,
};

KTEST(DeviceTest, MatchesIdsAndClass)
{
	device_t device = { .vendor_id = 0xfffe, .device_id = 0x1234 };
	EXPECT_TRUE(driver_match_pci(&mock_driver, &device) == &mock_ids[0]);
	device.device_id++;
	EXPECT_NULL(driver_match_pci(&mock_driver, &device));
	device.type = 0x0101;
	EXPECT_TRUE(driver_match_pci(&mock_driver, &device) == &mock_ids[1]);
	device.type = 0x0106;
	EXPECT_NULL(driver_match_pci(&mock_driver, &device));
	return 0;
}

KTEST(DeviceTest, ProbesOnceAndRecordsFailure)
{
	device_t device = {
		.vendor_id = 0xfffe,
		.device_id = 0x1234,
		.selected_driver = &mock_driver,
	};
	probe_calls = 0;
	probe_result = 0;
	device_probe(&device);
	device_probe(&device);
	EXPECT_EQ(probe_calls, 1);
	EXPECT_TRUE(device.driver == &mock_driver);
	EXPECT_EQ(device.probe_error, 0);

	device.driver = NULL;
	device.probe_done = 0;
	probe_result = -ENODEV;
	device_probe(&device);
	device_probe(&device);
	EXPECT_EQ(probe_calls, 2);
	EXPECT_NULL(device.driver);
	EXPECT_EQ(device.probe_error, -ENODEV);
	return 0;
}

KTEST(DeviceTest, DuplicateDriverRegistration)
{
	driver_t *driver;
	unsigned count = 0, matches = 0;
	driver_register(&mock_driver);
	driver_register(&mock_driver);
	for (driver = driver_first(); driver && count < 256;
	     driver = driver->next, count++)
		if (driver == &mock_driver)
			matches++;
	EXPECT_NULL(driver);
	EXPECT_EQ(matches, 1);
	return 0;
}

KTEST(DeviceTest, ScanPreservesBootInventory)
{
	const device_t *first = device_first(), *device;
	unsigned count = 0, after = 0;
	for (device = first; device; device = device->next)
		count++;
	pci_scan();
	EXPECT_TRUE(first == device_first());
	for (device = device_first(); device; device = device->next) {
		after++;
		if (device->selected_driver)
			EXPECT_TRUE(device->probe_done);
		if (device->driver)
			EXPECT_EQ(device->probe_error, 0);
	}
	EXPECT_EQ(count, after);
	return 0;
}

static int mock_ps2_probe(unsigned port)
{
	probe_calls++;
	return port == PS2_PORT_AUX ? 0 : -ENODEV;
}

KTEST(DeviceTest, BusIdentitySeparatesMatching)
{
	driver_t ps2_driver = {
		.bus = DEVICE_BUS_PS2,
		.ps2_port = PS2_PORT_AUX,
		.probe_ps2 = mock_ps2_probe,
	};
	device_t device = {
		.bus = DEVICE_BUS_PS2,
		.address = PS2_PORT_AUX,
		.vendor_id = 0xfffe,
		.device_id = 0x1234,
		.selected_driver = &ps2_driver,
	};
	/* PCI ID collisions cannot match devices on another bus. */
	EXPECT_NULL(driver_match_pci(&mock_driver, &device));
	probe_calls = 0;
	device_probe(&device);
	device_probe(&device);
	EXPECT_EQ(probe_calls, 1);
	EXPECT_TRUE(device.driver == &ps2_driver);
	device.probe_done = 0;
	device.driver = NULL;
	device.bus = DEVICE_BUS_PCI;
	device_probe(&device);
	EXPECT_EQ(probe_calls, 1);
	EXPECT_NULL(device.driver);
	EXPECT_EQ(device.probe_error, -ENODEV);
	return 0;
}

static void count_pci(uint32_t address, uint16_t vendor, uint16_t id,
		      void *data)
{
	(*(unsigned *)data)++;
}

KTEST(DeviceTest, PciQueriesExcludePs2Ports)
{
	const device_t *device;
	unsigned expected = 0, actual = 0;
	for (device = device_first(); device; device = device->next)
		if (device->bus == DEVICE_BUS_PCI)
			expected++;
	pci_for_each(count_pci, PCI_SCAN_ALL, &actual);
	EXPECT_EQ(actual, expected);
	const device_t *keyboard = ps2_device(PS2_PORT_KEYBOARD);
	const device_t *mouse = ps2_device(PS2_PORT_AUX);
	ps2_scan();
	EXPECT_TRUE(keyboard == ps2_device(PS2_PORT_KEYBOARD));
	EXPECT_TRUE(mouse == ps2_device(PS2_PORT_AUX));
	return 0;
}

KTEST(DeviceTest, SelectionDoesNotDependOnRegistrationOrder)
{
	/* Test-only IDs keep permanent fixtures separate from physical devices. */
	static const pci_device_id ids[] = {
		{ .vendor_id = 0xfffd, .device_id = 0x1000 },
		{ .vendor_id = 0xfffd, .device_id = 0x1001 },
		{ .vendor_id = 0xfffc, .device_id = 0x1000 },
		{ .vendor_id = 0xfffc, .device_id = 0x1001 },
	};
	static driver_t drivers[] = {
		{ .name = "test-order-a",
		  .bus = DEVICE_BUS_PCI,
		  .pci_ids = &ids[0],
		  .pci_id_count = 2,
		  .probe_pci = mock_probe },
		{ .name = "test-order-b",
		  .bus = DEVICE_BUS_PCI,
		  .pci_ids = &ids[0],
		  .pci_id_count = 1,
		  .probe_pci = mock_probe },
		{ .name = "test-order-c",
		  .bus = DEVICE_BUS_PCI,
		  .pci_ids = &ids[2],
		  .pci_id_count = 2,
		  .probe_pci = mock_probe },
		{ .name = "test-order-d",
		  .bus = DEVICE_BUS_PCI,
		  .pci_ids = &ids[2],
		  .pci_id_count = 1,
		  .probe_pci = mock_probe },
	};
	device_t device = { .bus = DEVICE_BUS_PCI, .vendor_id = 0xfffd };
	driver_register(&drivers[0]);
	driver_register(&drivers[1]);
	driver_register(&drivers[3]);
	driver_register(&drivers[2]);
	probe_calls = 0;
	device.device_id = 0x1001;
	EXPECT_TRUE(driver_select(&device) == &drivers[0]);
	device.device_id = 0x1000;
	EXPECT_NULL(driver_select(&device));
	device.vendor_id = 0xfffc;
	EXPECT_NULL(driver_select(&device));
	device.device_id = 0x1001;
	EXPECT_TRUE(driver_select(&device) == &drivers[2]);
	device.device_id = 0x1002;
	EXPECT_NULL(driver_select(&device));
	EXPECT_EQ(probe_calls, 0);
	return 0;
}

KTEST(DeviceTest, MultipleIdMatchesStillSelectOneDriver)
{
	static const pci_device_id ids[] = {
		{ .vendor_id = 0xfffb, .device_id = 0x1234 },
		{ .vendor_id = 0xfffb,
		  .device_id = PCI_ANY_ID,
		  .class_code = 0xfe01,
		  .class_mask = 0xffff },
	};
	static driver_t driver = {
		.name = "test-multiple-ids",
		.bus = DEVICE_BUS_PCI,
		.pci_ids = ids,
		.pci_id_count = 2,
		.probe_pci = mock_probe,
	};
	device_t device = { .bus = DEVICE_BUS_PCI,
			    .vendor_id = 0xfffb,
			    .device_id = 0x1234,
			    .type = 0xfe01 };
	driver_register(&driver);
	/* Both the exact ID and class entry match this single descriptor. */
	EXPECT_TRUE(driver_select(&device) == &driver);
	return 0;
}

KTEST(DeviceTest, ConflictingPs2DriversRemainUnbound)
{
	/* A test-only port keeps these fixtures outside the i8042 port set. */
	static driver_t first = {
		.name = "test-ps2-a",
		.bus = DEVICE_BUS_PS2,
		.ps2_port = 42,
		.probe_ps2 = mock_ps2_probe,
	};
	static driver_t second = {
		.name = "test-ps2-b",
		.bus = DEVICE_BUS_PS2,
		.ps2_port = 42,
		.probe_ps2 = mock_ps2_probe,
	};
	device_t device = { .bus = DEVICE_BUS_PS2, .address = 42 };
	driver_register(&first);
	driver_register(&second);
	probe_calls = 0;
	device.selected_driver = driver_select(&device);
	EXPECT_NULL(device.selected_driver);
	device_probe(&device);
	EXPECT_NULL(device.driver);
	EXPECT_EQ(probe_calls, 0);
	return 0;
}
