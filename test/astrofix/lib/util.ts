export function helper(items: string[]): string[] {
  return items.map((item) => item.toUpperCase());
}

export function formatTitle(title: string): string {
  return `${title} | Fixture`;
}
