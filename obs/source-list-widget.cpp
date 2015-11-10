#include <QMouseEvent>

#include <vector>

#include "qt-wrappers.hpp"
#include "source-list-widget.hpp"

Q_DECLARE_METATYPE(OBSInput);

void SourceListWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		QListWidget::mouseDoubleClickEvent(event);
}

void SourceListWidget::dropEvent(QDropEvent *event)
{
	QListWidget::dropEvent(event);
	if (!event->isAccepted() || !count())
		return;

	auto GetSceneItem = [&](int i)
	{
		return item(i)->data(Qt::UserRole).value<OBSInput>();
	};

	std::vector<obs_input_t*> newOrder;
	newOrder.reserve(count());
	for (int i = count() - 1; i >= 0; i--)
		newOrder.push_back(GetSceneItem(i));

	auto UpdateOrderAtomically = [&](obs_scene_t *scene)
	{
		ignoreReorder = true;
		obs_scene_reorder_items(scene, newOrder.data(),
				newOrder.size());
		ignoreReorder = false;
	};
	using UpdateOrderAtomically_t = decltype(UpdateOrderAtomically);

	auto scene = obs_scene_from_parent(GetSceneItem(0));
	obs_scene_atomic_update(scene, [](void *data, obs_scene_t *scene)
	{
		(*static_cast<UpdateOrderAtomically_t*>(data))(scene);
	}, static_cast<void*>(&UpdateOrderAtomically));
}
