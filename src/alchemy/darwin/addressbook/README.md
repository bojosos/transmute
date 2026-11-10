# AddressBook

`/System/Library/Frameworks/AddressBook.framework/Versions/A/AddressBook` (alchemy-fx).

* **Real:** the user's address book, which is empty. Alchemy keeps no contacts and does not read the
  host's.
  * `ABGetSharedAddressBook` and `+[ABAddressBook sharedAddressBook]` return the shared address book.
  * Its people and groups (`ABCopyArrayOfAllPeople`, `ABCopyArrayOfAllGroups`, `-people`, `-groups`)
    are empty arrays, and it has no "me" record.
  * `ABCopyLocalizedPropertyOrLabel` returns the name it is given: no localized names are known.
* **Value is ours, unverified:** the `kAB*` property and record type names (`kABFirstNameProperty`,
  `kABPersonRecordType`, ...) each hold their own symbol name. Apple's public documentation does not
  give their values. They work as opaque keys over a store that is always empty.
* **Stops explicitly:**
  * The record and multi-value calls (`ABRecordCopyValue`, `ABGroupCopyArrayOfAllMembers`,
    `ABMultiValue*`) answer NULL or 0 for NULL. Given anything else they stop, since no record can
    exist.
  * `ABTypeOfProperty` and `ABMultiValuePropertyType` stop: the values of `ABPropertyType` are not in
    Apple's public documentation.
  * `ABCopyArrayOfPropertiesForRecordType` stops: Alchemy defines only the names applications have
    linked so far, not a record type's full set of properties.
* **Host dependencies:** none.
* **Registration:** `install_addressbook`, one row of the installer table with the fill-only
  frameworks (through `gui::values::Provider`). Firefox's XUL and VLC's interface plug-in link the
  framework; LibreOffice's address book driver (`libmacabdrv1`) calls the C API.
* **Tests:** `tests/addressbook_tests.cpp` (ctest `alchemy_gui_addressbook`).
