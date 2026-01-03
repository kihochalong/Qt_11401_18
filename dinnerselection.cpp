#include "dinnerselection.h"
#include "ui_dinnerselection.h"
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>

DinnerSelection::DinnerSelection(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::DinnerSelection)
{
    ui->setupUi(this);
    connect(ui->horizontalSlider,
            &QSlider::valueChanged,
            this,
            &DinnerSelection::snapSliderToStep);
    connect(ui->btnPlus,  &QPushButton::clicked,
            this, &DinnerSelection::increasePrice);
    connect(ui->btnMinus, &QPushButton::clicked,
            this, &DinnerSelection::decreasePrice);
    connect(ui->checkBox, &QCheckBox::clicked, [=] {
        ui->checkBox_2->setChecked(false);
        ui->checkBox_3->setChecked(false);
    });
    connect(ui->checkBox_2, &QCheckBox::clicked, [=] {
        ui->checkBox->setChecked(false);
        ui->checkBox_3->setChecked(false);
    });
    connect(ui->checkBox_3, &QCheckBox::clicked, [=] {
        ui->checkBox->setChecked(false);
        ui->checkBox_2->setChecked(false);
    });
    connect(ui->sliderDistance, &QSlider::valueChanged,
            this, &DinnerSelection::onDistanceChanged);
    connect(ui->pushButton_4, &QPushButton::clicked, this, [=] {
        int v = ui->sliderDistance->value();
        if (v < ui->sliderDistance->maximum())
            ui->sliderDistance->setValue(v + 1);
    });
    connect(ui->pushButton_5, &QPushButton::clicked, this, [=] {
        int v = ui->sliderDistance->value();
        if (v > ui->sliderDistance->minimum())
            ui->sliderDistance->setValue(v - 1);
    });
    connect(ui->pushButton, &QPushButton::clicked,
            this, &DinnerSelection::applyFiltersAndShow);
    connect(ui->btnPick, &QPushButton::clicked, this, [=]() {
        if (currentFilteredRestaurants.isEmpty()) {
            ui->labelRandomResult->setText("🎲 隨機選取\n請先進行篩選");
            return;
        }
        int randomIndex = QRandomGenerator::global()->bounded(currentFilteredRestaurants.size());
        QJsonObject picked = currentFilteredRestaurants[randomIndex];

        QString name = picked["name"].toString();
        double rating = picked["rating"].toDouble(-1);

        ui->labelRandomResult->setText(
            QString("🎲 隨機選取：\n%1\n⭐ %2")
                .arg(name)
                .arg(rating < 0 ? "無" : QString::number(rating))
            );
    });
    mapWidget = new QQuickWidget(this);
    mapWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);

    mapWidget->setSource(QUrl("qrc:/map.qml"));

    ui->mapLayout->addWidget(mapWidget);
    ui->labelMap->hide();
    ui->listRestaurant->setStyleSheet(
        "QListWidget::item { border-bottom: 1px solid #C0C0C0; padding: 8px; }"
        );

    network = new QNetworkAccessManager(this);
    connect(network, &QNetworkAccessManager::finished,
            this, &DinnerSelection::onPlacesReply);
    fetchPlaces(23.7019, 120.4307);

}

DinnerSelection::~DinnerSelection()
{
    delete ui;
}

void DinnerSelection::snapSliderToStep(int value)
{
    const int step = 100;

    int snapped = (value + step / 2) / step * step;

    if (snapped != value) {
        ui->horizontalSlider->setValue(snapped);
    }
    ui->label_3->setText(QString("%1 元").arg(snapped));
}
void DinnerSelection::increasePrice()
{
    const int step = 100;

    int value = ui->horizontalSlider->value();
    int max   = ui->horizontalSlider->maximum();

    value += step;
    if (value > max) value = max;

    ui->horizontalSlider->setValue(value);
}

void DinnerSelection::decreasePrice()
{
    const int step = 100;

    int value = ui->horizontalSlider->value();
    int min   = ui->horizontalSlider->minimum();

    value -= step;
    if (value < min) value = min;

    ui->horizontalSlider->setValue(value);
}

void DinnerSelection::onDistanceChanged(int value)
{
    maxDistanceKm = value;
    ui->labelDistanceValue->setText(QString("%1 公里內").arg(value));
}

void DinnerSelection::fetchPlaces(double lat, double lon, QString pageToken)
{
    QUrl url("https://maps.googleapis.com/maps/api/place/nearbysearch/json");
    QUrlQuery query;

    QString apiKey = "AIzaSyBs73o60jvr_scDSieQsGLJCIUhKmoBoOw";
    query.addQueryItem("key", apiKey);

    if (!pageToken.isEmpty()) {
        query.addQueryItem("pagetoken", pageToken);
    } else {
        query.addQueryItem("location", QString("%1,%2").arg(lat).arg(lon));
        query.addQueryItem("radius", "2000"); // 預設抓取 2 公里內的原始資料，後續再由程式篩選
        query.addQueryItem("type", "restaurant");
        query.addQueryItem("language", "zh-TW");
    }

    url.setQuery(query);
    network->get(QNetworkRequest(url));
}

void DinnerSelection::onPlacesReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return;
    }

    QByteArray data = reply->readAll();
    QJsonObject root = QJsonDocument::fromJson(data).object();

    // 1. 將新結果「追加」進去，不要用 clear()
    QJsonArray results = root["results"].toArray();
    for (const QJsonValue &v : results) {
        allRestaurants.append(v.toObject());
    }

    // 2. 處理分頁
    m_nextPageToken = root["next_page_token"].toString();

    if (!m_nextPageToken.isEmpty()) {
        QTimer::singleShot(2000, this, [=]() {
            fetchPlaces(23.7019, 120.4307, m_nextPageToken);
        });
    } else {
        // 全部抓完才執行初次篩選顯示
        applyFiltersAndShow();
    }

    reply->deleteLater();
}

void DinnerSelection::applyFiltersAndShow()
{
    // --- 1. 計算目標價格等級 ---
    int sliderValue = ui->horizontalSlider->value();
    int targetPriceLevel = -1; // 預設 -1 代表不限金額

    if (sliderValue != 0) {
        if (sliderValue <= 200) targetPriceLevel = 1; // $100-200
        else targetPriceLevel = (sliderValue / 100) - 1; // 300->2, 400->3...
    }
    if (targetPriceLevel > 4) targetPriceLevel = 4;

    // --- 2. 初始化清單與抽籤池 ---
    ui->listRestaurant->clear();
    currentFilteredRestaurants.clear();

    // --- 3. 評分門檻設定 ---
    double minRatingThreshold = 0.0;
    bool isRatingSelected = ui->checkBox->isChecked() || ui->checkBox_2->isChecked() || ui->checkBox_3->isChecked();
    if (ui->checkBox_3->isChecked()) minRatingThreshold = 4.5;
    else if (ui->checkBox_2->isChecked()) minRatingThreshold = 4.0;
    else if (ui->checkBox->isChecked()) minRatingThreshold = 3.5;

    // --- 4. 進行過濾 ---
    for (const QJsonObject &obj : allRestaurants) {
        QString name = obj["name"].toString();
        double rating = obj["rating"].toDouble(-1);
        int priceLevel = obj["price_level"].toInt(-1);

        // A. 評分篩選：有選評分時，剔除「有分數且分數不夠」的店家
        if (isRatingSelected && rating >= 0 && rating < minRatingThreshold)
            continue;

        // B. 價格篩選：非 0 元時，剔除「有標註價格且價格不符」的店家
        if (targetPriceLevel != -1 && priceLevel != -1 && priceLevel != targetPriceLevel)
            continue;

        // C. 距離篩選
        if (!obj.contains("geometry")) continue;
        QJsonObject loc = obj["geometry"].toObject()["location"].toObject();
        double dLat = (loc["lat"].toDouble() - 23.7019) * 111.0;
        double dLon = (loc["lng"].toDouble() - 120.4307) * 111.0 * cos(23.7019 * M_PI / 180.0);
        double distanceKm = sqrt(dLat * dLat + dLon * dLon);

        if (distanceKm > maxDistanceKm) // maxDistanceKm 由距離 Slider 控制
            continue;

        // --- D. 通過所有門檻，存入結果 ---
        currentFilteredRestaurants.append(obj);

        QString avgPriceText;
        switch (priceLevel) {
        case 0: avgPriceText = "$1–100"; break;
        case 1: avgPriceText = "$100–200"; break;
        case 2: avgPriceText = "$200–300"; break;
        case 3: avgPriceText = "$300–500"; break;
        case 4: avgPriceText = "$500+"; break;
        default: avgPriceText = "未提供價格"; break;
        }

        ui->listRestaurant->addItem(
            QString("🍽 %1\n⭐ %2 | 💰 %3\n📍 %4 km")
                .arg(name)
                .arg(rating < 0 ? "無" : QString::number(rating))
                .arg(avgPriceText)
                .arg(QString::number(distanceKm, 'f', 2))
            );
    }

    if (currentFilteredRestaurants.isEmpty()) {
        ui->listRestaurant->addItem("⚠️ 沒有符合篩選條件的餐廳");
    }
}
