.PHONY: all test run clean install uninstall deb

all:
	$(MAKE) -C gtk-app all

test:
	@chmod +x run_tests.sh
	@./run_tests.sh

run:
	@./run.sh

clean:
	$(MAKE) -C gtk-app clean

install:
	$(MAKE) -C gtk-app install

uninstall:
	$(MAKE) -C gtk-app uninstall

deb:
	$(MAKE) -C gtk-app deb
