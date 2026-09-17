import 'package:flutter_test/flutter_test.dart';
import 'package:nlink_ng/main.dart';

void main() {
  testWidgets('home loads', (WidgetTester tester) async {
    await tester.pumpWidget(const NlinkNgApp());
    await tester.pump();
    expect(find.textContaining('nlink-ng'), findsWidgets);
  });
}
