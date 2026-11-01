/*
 * "Message a Friend" (Tell-A-Friend): pick a contact from the phone's address
 * book or type a number, and send an SMS challenge with the player's level and
 * gold.
 *
 * The PC has neither a BREW address book nor SMS. The menus and their flow are
 * kept: the address book reads as empty and sending takes the original's
 * failure path ("We were unable to send your message: Delivery failed").
 *
 * Menu.contacts holds 2 Strings per contact: [2i] name, [2i+1] number (the
 * original kept a pair of String pointers per contact, sorted by name).
 */

#include "game.h"

enum { TEXT_GROUP_MENU = 3 };

/* 0x00040f60 */
void Menu_freeContacts(Menu *menu)
{
	int n = menu->numContacts;

	for (int i = 0; i < n * 2; i++) {
		String_release(&menu->contacts[i]);
		String_dtor(&menu->contacts[i], 0);
	}
	free(menu->contacts);
	menu->contacts = NULL;
	menu->numContacts = 0;
}

/* 0x00040930 Read the address book. The original enumerated every record and
 * kept those with a name (field 5) and a phone number (field 0x16) that does
 * not start with '#', inserting them sorted by name. There is no address book
 * on PC, so the list stays empty (menu 0x98 then shows "no contacts"). */
void Menu_loadContacts(Menu *menu)
{
	int numRecords = 0; /* IAddrBook record count */

	Menu_freeContacts(menu);
	menu->numContacts = 0;
	menu->contacts = numRecords > 0 ? calloc((size_t)numRecords * 2, sizeof(String)) : NULL;
}

/* 0x00040cf0 One page of the address book: back/previous, next, title with
 * "page n of m", then the contacts (number shown as the help line). */
void Menu_buildContactsPage(Menu *menu)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	String empty, empty2;

	menu->contactsPerPage = (cv->viewRect[3] - 0x42) / 12 - 2;
	menu->numContactPages = (menu->numContacts + menu->contactsPerPage - 1) / menu->contactsPerPage;

	String_ctor(&empty);
	if (menu->contactPage == 1)
		MenuSystem_addItemIdHelpId(menu, 0x4c, &empty, 0, 0, 0x7f, 0x4d);
	else
		MenuSystem_addItemIdHelpId(menu, 0x14a, &empty, 0, 0, 0x7f, 0x14e);
	String_dtor(&empty, 0);

	String_ctor(&empty);
	if (menu->contactPage < menu->numContactPages) {
		MenuSystem_addItemIdHelpId(menu, 0x149, &empty, 0, 0, 0x7f, 0x14f);
	} else {
		String_ctor(&empty2);
		MenuSystem_addItem(menu, &empty, &empty2, 1);
		String_dtor(&empty2, 0);
	}
	String_dtor(&empty, 0);

	if (menu->numContacts < 1) {
		String_ctor(&empty);
		MenuSystem_addItemId(menu, 0x148, &empty, 1);
		String_dtor(&empty, 0);
		return;
	}

	Canvas_resetEvents(cv);
	Canvas_addTextArgInt(cv, menu->contactPage);
	Canvas_addTextArgInt(cv, menu->numContactPages);
	String *title = (String *)Canvas_buildDottedTitleId3(app->menuSystem, 0x150);
	String_ctor(&empty);
	MenuSystem_addItem(menu, title, &empty, 0x49);
	String_dtor(&empty, 0);

	int first = menu->contactsPerPage * (menu->contactPage - 1);
	for (int i = first, n = 0; i < menu->numContacts && n < menu->contactsPerPage; i++, n++) {
		String_ctor(&empty);
		MenuSystem_addItemFull(menu, &menu->contacts[i * 2], &empty, 0, 0, 0x7f, &menu->contacts[i * 2 + 1]);
		String_dtor(&empty, 0);
	}
	menu->contactFirst = first;
}

/* 0x00040b88 SMS-sent callback: frees the {message, number} String pair. */
void TellAFriend_sendDone(void *ctx)
{
	String *pair = ctx;

	if (pair == NULL)
		return;
	String_release(&pair[0]);
	String_release(&pair[1]);
	String_dtor(&pair[1], 0);
	String_dtor(&pair[0], 0);
	free(pair);
}

/* 0x00040bd0 Build the challenge text and send it to number. Returns 0x99 when
 * sent, 0x9a (with the reason in scratch2) when not. */
int Menu_sendTellAFriend(Menu *menu, String *number)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	String *msg = calloc(2, sizeof(String)); /* {text, number}, owned by the send callback */

	if (msg != NULL) {
		String_ctor(&msg[0]);
		String_ctor(&msg[1]);
	}
	Canvas_resetEvents(cv);
	Canvas_addTextArgInt(cv, app->player->level);
	Canvas_addTextArgInt(cv, app->player->gold);
	Text_getString(cv, TEXT_GROUP_MENU, 0x14d, &msg[0]);
	String_appendString(&msg[1], number);
	String_cstr(&msg[0]);
	String_cstr(&msg[1]);

	/* ITAPI SMS send: unavailable on PC, so it fails like a rejected request. */
	int sendResult = EFAILED;
	if (sendResult == SUCCESS)
		return 0x99; /* TellAFriend_sendDone frees msg when delivery completes */

	String_setLength(&menu->scratch2, 0);
	Text_getString(cv, TEXT_GROUP_MENU, 0x153, &menu->scratch2);
	/* the original leaked the block on this path; the port frees it */
	TellAFriend_sendDone(msg);
	return 0x9a;
}
