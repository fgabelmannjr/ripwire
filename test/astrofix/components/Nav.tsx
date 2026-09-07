import { helper } from '../lib/util';

export function Nav(): JSX.Element {
  const items = helper(['home', 'blog']);
  return (
    <nav>
      {items.map((item) => (
        <a key={item} href={`/${item}`}>
          {item}
        </a>
      ))}
    </nav>
  );
}

export default Nav;
