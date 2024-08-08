/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "payments/ui/payments_reaction_box.h"

#include "lang/lang_keys.h"
#include "ui/boxes/boost_box.h" // MakeBoostFeaturesBadge.
#include "ui/effects/premium_bubble.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/dynamic_image.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "styles/style_chat.h"
#include "styles/style_credits.h"
#include "styles/style_layers.h"
#include "styles/style_premium.h"
#include "styles/style_settings.h"

namespace Settings {
[[nodiscard]] not_null<Ui::RpWidget*> AddBalanceWidget(
	not_null<Ui::RpWidget*> parent,
	rpl::producer<uint64> balanceValue,
	bool rightAlign);
} // namespace Settings

namespace Ui {
namespace {

constexpr auto kMaxTopPaidShown = 3;

struct Discreter {
	Fn<int(float64)> ratioToValue;
	Fn<float64(int)> valueToRatio;
};

[[nodiscard]] Discreter DiscreterForMax(int max) {
	Expects(max >= 2);

	// 1/8 of width is 1..10
	// 1/3 of width is 1..100
	// 2/3 of width is 1..1000

	auto thresholds = base::flat_map<float64, int>();
	thresholds.emplace(0., 1);
	if (max <= 40) {
		thresholds.emplace(1., max);
	} else if (max <= 300) {
		thresholds.emplace(1. / 4, 10);
		thresholds.emplace(1., max);
	} else if (max <= 600) {
		thresholds.emplace(1. / 8, 10);
		thresholds.emplace(1. / 2, 100);
		thresholds.emplace(1., max);
	} else if (max <= 1900) {
		thresholds.emplace(1. / 8, 10);
		thresholds.emplace(1. / 3, 100);
		thresholds.emplace(1., max);
	} else {
		thresholds.emplace(1. / 8, 10);
		thresholds.emplace(1. / 3, 100);
		thresholds.emplace(2. / 3, 1000);
		thresholds.emplace(1., max);
	}

	const auto ratioToValue = [=](float64 ratio) {
		ratio = std::clamp(ratio, 0., 1.);
		const auto j = thresholds.lower_bound(ratio);
		if (j == begin(thresholds)) {
			return 1;
		}
		const auto i = j - 1;
		const auto progress = (ratio - i->first) / (j->first - i->first);
		const auto value = i->second + (j->second - i->second) * progress;
		return int(base::SafeRound(value));
	};
	const auto valueToRatio = [=](int value) {
		value = std::clamp(value, 1, max);
		auto i = begin(thresholds);
		auto j = i + 1;
		while (j->second < value) {
			i = j++;
		}
		const auto progress = (value - i->second)
			/ float64(j->second - i->second);
		return i->first + (j->first - i->first) * progress;
	};
	return {
		.ratioToValue = ratioToValue,
		.valueToRatio = valueToRatio,
	};
}

void PaidReactionSlider(
		not_null<VerticalLayout*> container,
		int current,
		int max,
		Fn<void(int)> changed) {
	Expects(current >= 1 && current <= max);

	const auto slider = container->add(
		object_ptr<MediaSlider>(container, st::paidReactSlider),
		st::boxRowPadding + QMargins(0, st::paidReactSliderTop, 0, 0));
	slider->resize(slider->width(), st::paidReactSlider.seekSize.height());

	const auto discreter = DiscreterForMax(max);
	slider->setAlwaysDisplayMarker(true);
	slider->setDirection(ContinuousSlider::Direction::Horizontal);
	slider->setValue(discreter.valueToRatio(current));
	slider->setAdjustCallback([=](float64 ratio) {
		return discreter.valueToRatio(discreter.ratioToValue(ratio));
	});
	const auto ratioToValue = discreter.ratioToValue;
	slider->setChangeProgressCallback([=](float64 value) {
		changed(ratioToValue(value));
	});
	slider->setChangeFinishedCallback([=](float64 value) {
		changed(ratioToValue(value));
	});
}

[[nodiscard]] QImage GenerateBadgeImage(int count) {
	const auto text = Lang::FormatCountDecimal(count);
	const auto length = st::chatSimilarBadgeFont->width(text);
	const auto contents = length
		+ st::chatSimilarLockedIcon.width();
	const auto badge = QRect(
		st::chatSimilarBadgePadding.left(),
		st::chatSimilarBadgePadding.top(),
		contents,
		st::chatSimilarBadgeFont->height);
	const auto rect = badge.marginsAdded(st::chatSimilarBadgePadding);

	auto result = QImage(
		rect.size() * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	result.fill(Qt::transparent);
	auto q = QPainter(&result);

	const auto &font = st::chatSimilarBadgeFont;
	const auto textTop = badge.y() + font->ascent;
	const auto icon = &st::chatSimilarLockedIcon;
	const auto position = st::chatSimilarLockedIconPosition;

	auto hq = PainterHighQualityEnabler(q);
	q.setBrush(st::creditsBg3);
	q.setPen(Qt::NoPen);
	const auto radius = rect.height() / 2.;
	q.drawRoundedRect(rect, radius, radius);

	auto textLeft = 0;
	if (icon) {
		icon->paint(
			q,
			badge.x() + position.x(),
			badge.y() + position.y(),
			rect.width());
		textLeft += position.x() + icon->width();
	}

	q.setFont(font);
	q.setPen(st::premiumButtonFg);
	q.drawText(textLeft, textTop, text);
	q.end();

	return result;
}

[[nodiscard]] not_null<RpWidget*> MakeTopReactor(
		not_null<QWidget*> parent,
		const PaidReactionTop &data) {
	const auto result = CreateChild<RpWidget>(parent);
	result->show();

	struct State {
		QImage badge;
		Text::String name;
	};
	const auto state = result->lifetime().make_state<State>();
	state->name.setText(st::defaultTextStyle, data.name);

	const auto count = data.count;
	const auto photo = data.photo;
	photo->subscribeToUpdates([=] {
		result->update();
	});
	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		state->badge = QImage();
	}, result->lifetime());
	result->paintRequest() | rpl::start_with_next([=] {
		auto p = Painter(result);
		const auto left = (result->width() - st::paidReactTopUserpic) / 2;
		p.drawImage(left, 0, photo->image(st::paidReactTopUserpic));

		if (state->badge.isNull()) {
			state->badge = GenerateBadgeImage(count);
		}
		const auto bwidth = state->badge.width()
			/ state->badge.devicePixelRatio();
		p.drawImage(
			(result->width() - bwidth) / 2,
			st::paidReactTopBadgeSkip,
			state->badge);

		p.setPen(st::windowFg);
		const auto skip = st::normalFont->spacew;
		const auto nameTop = st::paidReactTopNameSkip;
		const auto available = result->width() - skip * 2;
		state->name.draw(p, skip, nameTop, available, style::al_top);
	}, result->lifetime());

	return result;
}

void FillTopReactors(
		not_null<VerticalLayout*> container,
		std::vector<PaidReactionTop> top) {
	container->add(
		MakeBoostFeaturesBadge(
			container,
			tr::lng_paid_react_top_title(),
			[](QRect) { return st::creditsBg3->b; }),
		st::boxRowPadding + st::paidReactTopTitleMargin);

	const auto height = st::paidReactTopNameSkip + st::normalFont->height;
	const auto wrap = container->add(
		object_ptr<FixedHeightWidget>(container, height),
		st::paidReactTopMargin);
	struct State {
		std::vector<not_null<RpWidget*>> widgets;
	};
	const auto state = wrap->lifetime().make_state<State>();

	const auto topCount = std::min(int(top.size()), kMaxTopPaidShown);
	for (auto i = 0; i != topCount; ++i) {
		state->widgets.push_back(MakeTopReactor(wrap, top[i]));
	}

	wrap->widthValue() | rpl::start_with_next([=](int width) {
		const auto single = width / 4;
		if (single <= st::paidReactTopUserpic) {
			return;
		}
		auto left = (width - single * topCount) / 2;
		for (const auto widget : state->widgets) {
			widget->setGeometry(left, 0, single, height);
			left += single;
		}
	}, wrap->lifetime());
}

} // namespace

void PaidReactionsBox(
		not_null<GenericBox*> box,
		PaidReactionBoxArgs &&args) {
	args.max = std::max(args.max, 2);
	args.chosen = std::clamp(args.chosen, 1, args.max);

	box->setWidth(st::boxWideWidth);
	box->setStyle(st::paidReactBox);
	box->setNoContentMargin(true);

	struct State {
		rpl::variable<int> chosen;
	};
	const auto state = box->lifetime().make_state<State>();
	state->chosen = args.chosen;
	const auto changed = [=](int count) {
		state->chosen = count;
	};

	const auto content = box->verticalLayout();
	AddSkip(content, st::boxTitleClose.height + st::paidReactBubbleTop);

	const auto valueToRatio = DiscreterForMax(args.max).valueToRatio;
	auto bubbleRowState = state->chosen.value() | rpl::map([=](int value) {
		const auto full = st::boxWideWidth
			- st::boxRowPadding.left()
			- st::boxRowPadding.right();
		const auto marker = st::paidReactSlider.seekSize.width();
		const auto start = marker / 2;
		const auto inner = full - marker;
		const auto correct = start + inner * valueToRatio(value);
		return Premium::BubbleRowState{
			.counter = value,
			.ratio = correct / full,
		};
	});
	Premium::AddBubbleRow(
		content,
		st::boostBubble,
		BoxShowFinishes(box),
		std::move(bubbleRowState),
		Premium::BubbleType::Credits,
		nullptr,
		&st::paidReactBubbleIcon,
		st::boxRowPadding);

	PaidReactionSlider(content, args.chosen, args.max, changed);

	box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

	box->addRow(
		object_ptr<FlatLabel>(
			box,
			tr::lng_paid_react_title(),
			st::boostCenteredTitle),
		st::boxRowPadding + QMargins(0, st::paidReactTitleSkip, 0, 0));
	const auto labelWrap = box->addRow(
		object_ptr<RpWidget>(box),
		(st::boxRowPadding
			+ QMargins(0, st::lineWidth, 0, st::boostBottomSkip)));
	const auto label = CreateChild<FlatLabel>(
		labelWrap,
		(args.already
			? tr::lng_paid_react_already(
				lt_count,
				rpl::single(args.already) | tr::to_count(),
				Text::RichLangValue)
			: tr::lng_paid_react_about(
				lt_channel,
				rpl::single(Text::Bold(args.channel)),
				Text::RichLangValue)),
		st::boostText);
	labelWrap->widthValue() | rpl::start_with_next([=](int width) {
		label->resizeToWidth(width);
	}, label->lifetime());
	label->heightValue() | rpl::start_with_next([=](int height) {
		const auto min = 2 * st::normalFont->height;
		const auto skip = std::max((min - height) / 2, 0);
		labelWrap->resize(labelWrap->width(), 2 * skip + height);
		label->moveToLeft(0, skip);
	}, label->lifetime());

	if (!args.top.empty()) {
		FillTopReactors(content, std::move(args.top));
	}

	const auto button = box->addButton(rpl::single(QString()), [=] {
		args.send(state->chosen.current());
	});
	{
		const auto buttonLabel = CreateChild<FlatLabel>(
			button,
			rpl::single(QString()),
			st::creditsBoxButtonLabel);
		args.submit(
			state->chosen.value()
		) | rpl::start_with_next([=](const TextWithContext &text) {
			buttonLabel->setMarkedText(
				text.text,
				text.context);
		}, buttonLabel->lifetime());
		buttonLabel->setTextColorOverride(
			box->getDelegate()->style().button.textFg->c);
		button->sizeValue(
		) | rpl::start_with_next([=](const QSize &size) {
			buttonLabel->moveToLeft(
				(size.width() - buttonLabel->width()) / 2,
				(size.height() - buttonLabel->height()) / 2);
		}, buttonLabel->lifetime());
		buttonLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	box->widthValue(
	) | rpl::start_with_next([=](int width) {
		const auto &padding = st::paidReactBox.buttonPadding;
		button->resizeToWidth(width
			- padding.left()
			- padding.right());
		button->moveToLeft(padding.left(), button->y());
	}, button->lifetime());

	{
		const auto balance = Settings::AddBalanceWidget(
			content,
			std::move(args.balanceValue),
			false);
		rpl::combine(
			balance->sizeValue(),
			box->widthValue()
		) | rpl::start_with_next([=] {
			balance->moveToLeft(
				st::creditsHistoryRightSkip * 2,
				st::creditsHistoryRightSkip);
			balance->update();
		}, balance->lifetime());
	}
}

object_ptr<BoxContent> MakePaidReactionBox(PaidReactionBoxArgs &&args) {
	return Box(PaidReactionsBox, std::move(args));
}

} // namespace Ui