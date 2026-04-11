#include "linux/StatusNotifierItem.h"

#include <cstring>
#include <libappindicator/app-indicator.h>
#include <iostream>

TrayIcon *TrayIcon::getNew(TrayIconData applicationName, std::function<void()> &&beforeShow)
{
	return new StatusNotifierItem(applicationName, std::forward<std::function<void()>>(beforeShow));
}

StatusNotifierItem::StatusNotifierItem(TrayIconData, std::function<void()> &&beforeShow)
  : thread_{ [this, beforeShow = std::move(beforeShow)] {
	  // Initialize GTK before creating any widgets
	  int argc = 0;
	  char **argv = nullptr;
	  
	  if (!gtk_init_check(&argc, &argv))
	  {
		  std::cerr << "[Tray] Failed to initialize GTK" << std::endl;
		  return;
	  }

	  // Set up icon path
	  std::string iconPath{};
	  const auto APPDIR = ::getenv("APPDIR");
	  if (APPDIR != nullptr)
	  {
		  iconPath = APPDIR;
		  iconPath += "/usr/share/icons/hicolor/24x24/status/jsm-status-dark.svg";
		  gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(), iconPath.c_str());
	  }
	  else
	  {
		  iconPath = "jsm-status-dark";
	  }

	  // Create menu BEFORE creating the indicator
	  menu_ = std::unique_ptr<GtkMenu, decltype(&::g_object_unref)>{ GTK_MENU(gtk_menu_new()), &g_object_unref };
	  
	  if (!menu_)
	  {
		  std::cerr << "[Tray] Failed to create menu" << std::endl;
		  return;
	  }

	  // Create indicator
	  indicator_ = app_indicator_new(APPLICATION_RDN APPLICATION_NAME, iconPath.c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
	  
	  if (!indicator_)
	  {
		  std::cerr << "[Tray] Failed to create app indicator" << std::endl;
		  return;
	  }

	  app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
	  app_indicator_set_menu(indicator_, menu_.get());

	  // Call the beforeShow callback to populate the menu
	  beforeShow();

	  // Show the menu and all widgets
	  gtk_widget_show_all(GTK_WIDGET(menu_.get()));

	  // Now run the GTK main loop
	  gtk_main();
  } }
{
}

StatusNotifierItem::~StatusNotifierItem()
{
	// Schedule GTK main loop to quit from within the GTK thread
	g_idle_add([](void *) -> gboolean {
		gtk_main_quit();
		return FALSE;
	},
	  nullptr);

	// Wait for the thread to finish
	thread_.join();
}

bool StatusNotifierItem::Show()
{
	// Schedule the show operation in the GTK thread
	g_idle_add([](void *self) -> gboolean {
		auto *item = static_cast<StatusNotifierItem *>(self);
		if (item->menu_)
		{
			gtk_widget_show_all(GTK_WIDGET(item->menu_.get()));
		}
		return FALSE;
	},
	  this);

	return true;
}

bool StatusNotifierItem::Hide()
{
	// Schedule the hide operation in the GTK thread
	g_idle_add([](void *self) -> gboolean {
		auto *item = static_cast<StatusNotifierItem *>(self);
		if (item->menu_)
		{
			gtk_widget_hide(GTK_WIDGET(item->menu_.get()));
		}
		return FALSE;
	},
	  this);

	return true;
}

bool StatusNotifierItem::SendNotification(const std::string &)
{
	return true;
}

void StatusNotifierItem::AddMenuItem(const std::string &label, ClickCallbackType &&onClick)
{
	// Disable show-hide console since this is not supported on Linux
	if (label == "Show Console")
		return;

	g_idle_add([](void *data) -> gboolean {
		auto *context = static_cast<MenuItemAddContext *>(data);
		
		auto item = context->self;
		const auto &label = context->label;
		auto onClick = context->onClick;

		item->menuItems_.emplace_back(GTK_MENU_ITEM(gtk_menu_item_new_with_label(label.c_str())));
		auto &menuItem = item->menuItems_.back();

		item->callbacks_.emplace_back(onClick);
		auto &cb = item->callbacks_.back();

		g_signal_connect(menuItem, "activate", G_CALLBACK(&StatusNotifierItem::OnActivate), const_cast<ClickCallbackType *>(&cb));

		gtk_container_add(GTK_CONTAINER(item->menu_.get()), GTK_WIDGET(menuItem));
		gtk_widget_show_all(GTK_WIDGET(menuItem));

		delete context;
		return FALSE;
	},
	  new MenuItemAddContext{this, label, onClick});

	return;
}

void StatusNotifierItem::AddMenuItem(const std::string &label, ClickCallbackTypeChecked &&onClick, StateCallbackType &&getState)
{
	// Disable show-hide console since this is not supported on Linux
	if (label == "Show Console")
		return;

	g_idle_add([](void *data) -> gboolean {
		auto *context = static_cast<MenuItemAddCheckedContext *>(data);
		
		auto item = context->self;
		const auto &label = context->label;
		auto onClick = context->onClick;
		auto getState = context->getState;

		item->menuItems_.emplace_back(GTK_MENU_ITEM(gtk_check_menu_item_new_with_label(label.c_str())));
		auto &menuItem = item->menuItems_.back();

		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menuItem), getState());

		// Create a wrapper callback that combines onClick and getState
		item->callbacks_.emplace_back([onClick, getState]() {
			onClick(!getState());
		});
		auto &cb = item->callbacks_.back();

		g_signal_connect(menuItem, "activate", G_CALLBACK(&StatusNotifierItem::OnActivate), const_cast<ClickCallbackType *>(&cb));

		gtk_container_add(GTK_CONTAINER(item->menu_.get()), GTK_WIDGET(menuItem));
		gtk_widget_show_all(GTK_WIDGET(menuItem));

		delete context;
		return FALSE;
	},
	  new MenuItemAddCheckedContext{this, label, std::move(onClick), std::move(getState)});

	return;
}

void StatusNotifierItem::AddMenuItem(const std::string &l, const std::string &sl, ClickCallbackType &&onClick)
{
	// Disable show-hide console since this is not supported on Linux
	if (l == "Show Console")
		return;

	g_idle_add([](void *data) -> gboolean {
		auto *context = static_cast<MenuItemAddSubContext *>(data);
		
		auto item = context->self;
		const auto &label = context->label;
		const auto &subLabel = context->subLabel;
		auto onClick = context->onClick;

		const char *labelCStr = label.c_str();
		const char *subLabelCStr = subLabel.c_str();

		const auto it = std::find_if(item->menuItems_.begin(), item->menuItems_.end(), [&labelCStr](GtkMenuItem *menuItem) {
			return strcmp(labelCStr, gtk_menu_item_get_label(menuItem)) == 0;
		});

		GtkMenuItem *menuItem = nullptr;

		if (it == item->menuItems_.end())
		{
			item->menuItems_.emplace_back(GTK_MENU_ITEM(gtk_menu_item_new_with_label(labelCStr)));
			menuItem = item->menuItems_.back();
			gtk_container_add(GTK_CONTAINER(item->menu_.get()), GTK_WIDGET(menuItem));
			gtk_widget_show_all(GTK_WIDGET(menuItem));
		}
		else
		{
			menuItem = *it;
		}

		auto subMenuIt = item->subMenus_.find(menuItem);
		if (subMenuIt == item->subMenus_.end())
		{
			subMenuIt = item->subMenus_.emplace(menuItem, std::pair<GtkMenu *, std::vector<GtkMenuItem *>>{GTK_MENU(gtk_menu_new()), std::vector<GtkMenuItem *>{}}).first;
			gtk_menu_item_set_submenu(menuItem, GTK_WIDGET(subMenuIt->second.first));
			gtk_widget_show_all(GTK_WIDGET(menuItem));
		}

		auto &subMenu = subMenuIt->second.first;
		auto &subMenuItems = subMenuIt->second.second;

		subMenuItems.emplace_back(GTK_MENU_ITEM(gtk_check_menu_item_new_with_label(subLabelCStr)));
		GtkMenuItem *subMenuItemPtr = subMenuItems.back();
		auto *subMenuItemsPtr = &subMenuItems;

		item->callbacks_.emplace_back([onClick, subMenuItemsPtr, subMenuItemPtr]() {
			// Block "activate" signal handlers on all sub-items before changing check
			// states. gtk_check_menu_item_set_active() internally calls
			// gtk_menu_item_activate() which re-emits "activate", causing infinite
			// recursion if not blocked.
			auto onActivateFn = reinterpret_cast<gpointer>(reinterpret_cast<GCallback>(&StatusNotifierItem::OnActivate));
			for (auto *si : *subMenuItemsPtr)
			{
				g_signal_handlers_block_matched(G_OBJECT(si), G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, onActivateFn, nullptr);
				gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(si), FALSE);
			}
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(subMenuItemPtr), TRUE);
			for (auto *si : *subMenuItemsPtr)
			{
				g_signal_handlers_unblock_matched(G_OBJECT(si), G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, onActivateFn, nullptr);
			}
			onClick();
		});
		auto &cb = item->callbacks_.back();
		g_signal_connect(subMenuItemPtr, "activate", G_CALLBACK(&StatusNotifierItem::OnActivate), const_cast<ClickCallbackType *>(&cb));

		gtk_container_add(GTK_CONTAINER(subMenu), GTK_WIDGET(subMenuItemPtr));
		gtk_widget_show_all(GTK_WIDGET(subMenuItemPtr));

		delete context;
		return FALSE;
	},
	  new MenuItemAddSubContext{this, l, sl, onClick});

	return;
}

void StatusNotifierItem::ClearMenuMap()
{
	// The idle callback captures 'this'. This is safe because the destructor
	// calls thread_.join() which blocks until the GTK thread (and all pending
	// idle callbacks) finishes, so 'this' is always valid while the callback runs.
	g_idle_add([](void *self) -> gboolean {
		auto *item = static_cast<StatusNotifierItem *>(self);

		if (item->menu_)
		{
			// gtk_widget_destroy disconnects all signal handlers and removes the
			// widget from its parent container, so it is safe to clear callbacks_
			// afterwards without risking dangling GTK signal data pointers.
			for (auto *menuItem : item->menuItems_)
			{
				gtk_widget_destroy(GTK_WIDGET(menuItem));
			}
		}

		item->menuItems_.clear();
		item->subMenus_.clear();
		item->callbacks_.clear();

		return FALSE;
	},
	  this);
}

void StatusNotifierItem::OnActivate(GtkMenuItem *, void *data) noexcept
{
	auto *cb = static_cast<ClickCallbackType *>(data);
	(*cb)();
}

StatusNotifierItem::operator bool()
{
	return indicator_ != nullptr;
}
