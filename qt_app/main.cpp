#include "app_style.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QFont>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QFont font = app.font();
    font.setPointSize(10);
    app.setFont(font);
    app.setStyleSheet(appStyleSheet());

    MainWindow window;
    window.show();

    return app.exec();
}
